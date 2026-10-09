// Crash reporter. On an unhandled SEH exception it prints the
// exception, the faulting thread, module+offset and symbol (wg3d.exe links with /DEBUG, so recompiled
// functions resolve to their MIPS names, e.g. func_80012345), a native stack walk, the N64 address
// for faults inside the RDRAM reservation, and the thread's trace ring (--trace builds).
// dump_all_threads() prints every thread's stack, for hangs (WG3D_DUMP_AFTER=<seconds> runs it once).
// The report also goes to logs/crash-<date>-<time>.txt, and a dialog points at it (unless
// dialogs are off, as in --frames test runs). WG3D_CRASH_AFTER=<seconds> crashes on purpose, for testing.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>
#include <TlHelp32.h>
#endif

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include "wg3d_crash.h"
#include "wg3d_log.h"
#include "wg3d_trace.h"

namespace {
    uint8_t* rdram_base = nullptr;
    std::atomic<bool> dialogs{ true };
    // librecomp reserves 4GB of address space for RDRAM (MEM macros index rdram + (addr - 0x80000000)).
    constexpr uint64_t RdramReserve = 0x100000000ULL;
}

void wg3d::crash::set_rdram(uint8_t* rdram) {
    rdram_base = rdram;
}

void wg3d::crash::set_dialogs(bool enabled) {
    dialogs = enabled;
}

#ifdef _WIN32
namespace {
    std::mutex dbghelp_mutex;
    bool sym_initialized = false;

    const char* code_name(DWORD code) {
        switch (code) {
            case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
            case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
            case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
            case EXCEPTION_INT_OVERFLOW: return "INT_OVERFLOW";
            case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
            case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
            case EXCEPTION_FLT_INVALID_OPERATION: return "FLT_INVALID_OPERATION";
            case EXCEPTION_BREAKPOINT: return "BREAKPOINT";
            case 0xE06D7363: return "C++ exception";
            default: return "unknown";
        }
    }

    void init_symbols(HANDLE process) {
        if (!sym_initialized) {
            SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
            SymInitialize(process, nullptr, TRUE);
            sym_initialized = true;
        }
    }

    void describe_address(FILE* out, HANDLE process, uint64_t addr) {
        HMODULE module = nullptr;
        char module_path[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(addr), &module)) {
            GetModuleFileNameA(module, module_path, sizeof(module_path));
        }
        const char* module_name = module_path;
        for (const char* p = module_path; *p; p++) {
            if (*p == '\\' || *p == '/') module_name = p + 1;
        }

        alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO* sym = reinterpret_cast<SYMBOL_INFO*>(sym_buf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        std::fprintf(out, "0x%016" PRIx64 "  %s+0x%" PRIx64, addr, module_name,
                     module ? addr - reinterpret_cast<uint64_t>(module) : 0);
        if (SymFromAddr(process, addr, &disp, sym)) {
            std::fprintf(out, "  %s+0x%" PRIx64, sym->Name, static_cast<uint64_t>(disp));
        }
        std::fputc('\n', out);
    }

    void walk_stack(FILE* out, HANDLE process, HANDLE thread, CONTEXT walk_ctx, int max_frames) {
        STACKFRAME64 frame{};
        frame.AddrPC.Offset = walk_ctx.Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = walk_ctx.Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Offset = walk_ctx.Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        for (int i = 0; i < max_frames; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &walk_ctx, nullptr,
                             SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0) {
                break;
            }
            std::fprintf(out, "  #%02d ", i);
            describe_address(out, process, frame.AddrPC.Offset);
        }
    }

    void print_thread_name(FILE* out, HANDLE thread) {
        PWSTR desc = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &desc)) && desc) {
            if (desc[0]) std::fprintf(out, " \"%ls\"", desc);
            LocalFree(desc);
        }
    }

    void write_report(FILE* out, EXCEPTION_POINTERS* info) {
        HANDLE process = GetCurrentProcess();
        const EXCEPTION_RECORD* rec = info->ExceptionRecord;
        CONTEXT* ctx = info->ContextRecord;

        std::fprintf(out, "\n[wg3d] ==== CRASH ====\n");
        std::fprintf(out, "[wg3d] exception 0x%08lX (%s) on thread %lu", rec->ExceptionCode, code_name(rec->ExceptionCode),
                     GetCurrentThreadId());
        print_thread_name(out, GetCurrentThread());
        std::fputc('\n', out);

        std::fprintf(out, "[wg3d] at ");
        describe_address(out, process, reinterpret_cast<uint64_t>(rec->ExceptionAddress));

        if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
            uint64_t target = rec->ExceptionInformation[1];
            const char* kind = rec->ExceptionInformation[0] == 0 ? "read" : rec->ExceptionInformation[0] == 1 ? "write" : "execute";
            std::fprintf(out, "[wg3d] %s of 0x%016" PRIx64, kind, target);
            uint64_t base = reinterpret_cast<uint64_t>(rdram_base);
            if (rdram_base && target >= base && target < base + RdramReserve) {
                std::fprintf(out, "  = N64 address 0x%08" PRIX64 " (rdram offset 0x%" PRIX64 ")",
                             (target - base + 0x80000000ULL) & 0xFFFFFFFFULL, target - base);
            }
            std::fputc('\n', out);
        }

        std::fprintf(out, "[wg3d] rax=%016llx rbx=%016llx rcx=%016llx rdx=%016llx\n", ctx->Rax, ctx->Rbx, ctx->Rcx, ctx->Rdx);
        std::fprintf(out, "[wg3d] rsi=%016llx rdi=%016llx rbp=%016llx rsp=%016llx\n", ctx->Rsi, ctx->Rdi, ctx->Rbp, ctx->Rsp);
        std::fprintf(out, "[wg3d] r8 =%016llx r9 =%016llx r10=%016llx r11=%016llx\n", ctx->R8, ctx->R9, ctx->R10, ctx->R11);
        std::fprintf(out, "[wg3d] r12=%016llx r13=%016llx r14=%016llx r15=%016llx\n", ctx->R12, ctx->R13, ctx->R14, ctx->R15);
        if (rdram_base) {
            std::fprintf(out, "[wg3d] rdram base=%p\n", static_cast<void*>(rdram_base));
        }

        // Native stack walk (works for optimized code via the x64 unwind tables).
        std::fprintf(out, "[wg3d] stack:\n");
        walk_stack(out, process, GetCurrentThread(), *ctx, 48);

        wg3d::trace::dump_current_thread(out);
        std::fprintf(out, "[wg3d] ==== END CRASH ====\n");
        std::fflush(out);
    }

    // logs/crash-YYYYMMDD-HHMMSS.txt, or empty if the log folder isn't set up.
    std::filesystem::path crash_file_path() {
        std::filesystem::path dir = wg3d::log::dir();
        if (dir.empty()) {
            return {};
        }
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t name[64];
        swprintf(name, 64, L"crash-%04u%02u%02u-%02u%02u%02u.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        return dir / name;
    }

    LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* info) {
        static volatile LONG entered = 0;
        if (InterlockedExchange(&entered, 1) != 0) {
            // Another thread is already reporting; let it finish.
            Sleep(INFINITE);
        }
        init_symbols(GetCurrentProcess());
        // The crash file first: it doesn't depend on the log pump threads still running.
        std::filesystem::path crash_path = crash_file_path();
        FILE* crash_file = crash_path.empty() ? nullptr : _wfopen(crash_path.c_str(), L"w");
        if (crash_file != nullptr) {
            write_report(crash_file, info);
            std::fclose(crash_file);
        }
        write_report(stderr, info);
        if (crash_file != nullptr) {
            std::fprintf(stderr, "[wg3d] crash report saved to %s\n", crash_path.string().c_str());
        }
        wg3d::log::flush();
        if (dialogs) {
            std::wstring msg = L"W.G. 3D Hockey: Recompiled crashed.";
            if (crash_file != nullptr) {
                msg += L"\n\nA report was saved to:\n" + crash_path.wstring();
            }
            MessageBoxW(nullptr, msg.c_str(), L"W.G. 3D Hockey: Recompiled", MB_OK | MB_ICONERROR | MB_TOPMOST);
        }
        return EXCEPTION_EXECUTE_HANDLER;
    }
}

void wg3d::crash::dump_all_threads(FILE* out) {
    std::lock_guard lock{ dbghelp_mutex };
    HANDLE process = GetCurrentProcess();
    init_symbols(process);
    DWORD pid = GetCurrentProcessId();
    DWORD self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }
    std::fprintf(out, "\n[wg3d] ==== THREAD DUMP ====\n");
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) {
            continue;
        }
        HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                   te.th32ThreadID);
        if (!thread) {
            continue;
        }
        std::fprintf(out, "[wg3d] thread %lu", te.th32ThreadID);
        print_thread_name(out, thread);
        std::fputc('\n', out);
        if (SuspendThread(thread) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_FULL;
            if (GetThreadContext(thread, &ctx)) {
                walk_stack(out, process, thread, ctx, 24);
            }
            ResumeThread(thread);
        }
        CloseHandle(thread);
    }
    CloseHandle(snap);
    std::fprintf(out, "[wg3d] ==== END THREAD DUMP ====\n");
    std::fflush(out);
}

void wg3d::crash::install() {
    SetUnhandledExceptionFilter(unhandled_filter);
    // Report instead of showing the Windows Error Reporting dialog.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (const char* after = std::getenv("WG3D_DUMP_AFTER")) {
        int seconds = std::atoi(after);
        std::thread{ [seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            wg3d::crash::dump_all_threads(stderr);
        } }.detach();
    }
    if (const char* after = std::getenv("WG3D_CRASH_AFTER")) {
        int seconds = std::atoi(after);
        std::thread{ [seconds]() {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            std::fprintf(stderr, "[wg3d] WG3D_CRASH_AFTER: crashing on purpose\n");
            *static_cast<volatile int*>(nullptr) = 0;
        } }.detach();
    }
}
#else
void wg3d::crash::install() {}
void wg3d::crash::set_dialogs(bool) {}
void wg3d::crash::dump_all_threads(FILE*) {}
#endif
