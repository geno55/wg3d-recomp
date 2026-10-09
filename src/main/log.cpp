// Log file and output forwarding. See include/wg3d_log.h.
//
// stdout and stderr (the CRT descriptors 1 and 2, the FILE streams, and the Win32 standard handles) are
// each pointed at a pipe. A pump thread per pipe copies what arrives into the shared log file and, if the
// process had somewhere to show it, to that original destination too.
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#endif

#include "wg3d_log.h"

namespace {
    constexpr int KeptLogs = 3;  // wg3d.log plus wg3d.1.log and wg3d.2.log
    std::filesystem::path log_dir;

#ifdef _WIN32
    struct Stream {
        HANDLE read = nullptr;
        HANDLE forward = nullptr;  // original destination, or nullptr
    };
    Stream streams[2];
    HANDLE log_file = INVALID_HANDLE_VALUE;
    std::mutex log_mutex;

    // The read end is overlapped: on a synchronous handle, flush()'s PeekNamedPipe would wait behind the
    // pump's blocked ReadFile forever (Windows serialises synchronous I/O per file object).
    DWORD WINAPI pump(void* param) {
        Stream* s = static_cast<Stream*>(param);
        char buf[4096];
        DWORD n = 0, written = 0;
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        for (;;) {
            ResetEvent(ov.hEvent);
            if (!ReadFile(s->read, buf, sizeof(buf), nullptr, &ov) && GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            if (!GetOverlappedResult(s->read, &ov, &n, TRUE) || n == 0) {
                break;
            }
            if (log_file != INVALID_HANDLE_VALUE) {
                std::lock_guard lock{ log_mutex };
                WriteFile(log_file, buf, n, &written, nullptr);
            }
            if (s->forward != nullptr) {
                WriteFile(s->forward, buf, n, &written, nullptr);
            }
        }
        return 0;
    }

    bool usable(HANDLE h) {
        return h != nullptr && h != INVALID_HANDLE_VALUE && GetFileType(h) != FILE_TYPE_UNKNOWN;
    }

    // A copy of the current standard handle (redirecting the CRT descriptor below closes the original),
    // or the console's output if there is no usable handle and a console was asked for.
    HANDLE forward_target(DWORD std_id, bool want_console) {
        HANDLE h = GetStdHandle(std_id);
        if (!usable(h)) {
            if (!want_console) {
                return nullptr;
            }
            static bool attached = AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole();
            if (!attached) {
                return nullptr;
            }
            return CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        }
        HANDLE copy = nullptr;
        DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return copy;
    }

    void redirect(int index, FILE* stream, int fd, DWORD std_id, bool want_console) {
        Stream& s = streams[index];
        s.forward = forward_target(std_id, want_console);
        wchar_t name[96];
        swprintf(name, 96, L"\\\\.\\pipe\\wg3d-log-%lu-%d", GetCurrentProcessId(), index);
        s.read = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                                  PIPE_TYPE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 64 * 1024, 0, nullptr);
        if (s.read == INVALID_HANDLE_VALUE) {
            s.read = nullptr;
            return;
        }
        HANDLE write = CreateFileW(name, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (write == INVALID_HANDLE_VALUE) {
            CloseHandle(s.read);
            s.read = nullptr;
            return;
        }
        int pipe_fd = _open_osfhandle(reinterpret_cast<intptr_t>(write), _O_WRONLY | _O_TEXT);
        // A GUI process starts with no stdout/stderr: give the FILE a descriptor before replacing it.
        if (_fileno(stream) < 0) {
            freopen("NUL", "w", stream);
        }
        _dup2(pipe_fd, _fileno(stream));
        if (_fileno(stream) != fd) {
            _dup2(pipe_fd, fd);
        }
        _close(pipe_fd);
        setvbuf(stream, nullptr, _IONBF, 0);
        SetStdHandle(std_id, reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stream))));
        HANDLE thread = CreateThread(nullptr, 0, pump, &s, 0, nullptr);
        if (thread != nullptr) {
            SetThreadDescription(thread, index == 0 ? L"Log pump (stdout)" : L"Log pump (stderr)");
            CloseHandle(thread);
        }
    }
#endif

    void rotate(const std::filesystem::path& dir) {
        std::error_code ec;
        auto name = [&](int i) { return dir / (i == 0 ? std::string("wg3d.log") : "wg3d." + std::to_string(i) + ".log"); };
        std::filesystem::remove(name(KeptLogs - 1), ec);
        for (int i = KeptLogs - 2; i >= 0; i--) {
            std::filesystem::rename(name(i), name(i + 1), ec);
        }
    }
}

void wg3d::log::init(const std::filesystem::path& app_folder, bool want_console) {
    std::error_code ec;
    log_dir = app_folder / "logs";
    std::filesystem::create_directories(log_dir, ec);
    rotate(log_dir);
#ifdef _WIN32
    log_file = CreateFileW((log_dir / "wg3d.log").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    redirect(0, stdout, 1, STD_OUTPUT_HANDLE, want_console);
    redirect(1, stderr, 2, STD_ERROR_HANDLE, want_console);
    std::atexit(flush);
#endif
}

void wg3d::log::flush() {
#ifdef _WIN32
    std::fflush(stdout);
    std::fflush(stderr);
    // Wait for both pipes to empty (at most ~1 s), then a moment for the pumps to finish writing.
    for (int i = 0; i < 100; i++) {
        DWORD pending = 0;
        for (const Stream& s : streams) {
            DWORD avail = 0;
            if (s.read != nullptr && PeekNamedPipe(s.read, nullptr, 0, nullptr, &avail, nullptr)) {
                pending += avail;
            }
        }
        if (pending == 0) {
            break;
        }
        Sleep(10);
    }
    Sleep(20);
#endif
}

std::filesystem::path wg3d::log::dir() {
    return log_dir;
}
