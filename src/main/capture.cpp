// In-process screenshots for the scenario harness.
//
// WG3D_CAPTURE="t1:label1,t2:label2,..." saves the window's client area at script time t (seconds on
// the WG3D_INPUT_SCRIPT clock, wg3d::input::script_time) to WG3D_CAPTURE_DIR/<label>.bmp (default: the
// working directory). Captures run on the main thread right after the event pump, so they are aligned
// with scripted input to within one main-loop iteration, unlike an external capture.
//
// PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) reads the DWM-composed window, so it works while
// other windows cover it.
//
// Fast runs (--speed N > 1): RT64 is the bottleneck at speed and the game's logic doesn't depend on what
// is drawn, so display lists are only rendered for RenderLead game seconds before each capture. On the
// capture's VI, on_vi() holds the VI thread (the game can't advance) until update() has taken the shot,
// so the capture shows exactly that game time.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <limits>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "SDL.h"
#include "SDL_syswm.h"

#include "wg3d_capture.h"
#include "wg3d_input.h"
#include "wg3d_stats.h"

namespace {
    struct Capture {
        double time;
        std::string label;
        bool done = false;
    };
    std::vector<Capture> captures;
    std::filesystem::path out_dir = ".";
    // Captures are taken in time order; this is the time of the next one (main thread writes, gfx thread
    // reads). Infinity once all are done.
    std::atomic<double> next_capture{ std::numeric_limits<double>::infinity() };
    std::atomic<bool> render_only_for_captures{ false };
    // Game seconds rendered before each capture, so RT64's framebuffers and texture cache are warm.
    constexpr double RenderLead = 0.5;
    // Fast runs: how long on_vi waits for the held frame to be rendered and presented before capturing.
    constexpr int PresentWaitMs = 60;
    std::mutex hold_mutex;
    std::condition_variable hold_cv;
    bool holding = false;  // on_vi is holding the game for a capture

#ifdef _WIN32
    bool save_bmp(HWND hwnd, const std::filesystem::path& path) {
        RECT rc{};
        GetClientRect(hwnd, &rc);
        const int w = rc.right - rc.left, h = rc.bottom - rc.top;
        if (w <= 0 || h <= 0) return false;
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateCompatibleBitmap(screen, w, h);
        HGDIOBJ old = SelectObject(mem, bmp);
        bool ok = PrintWindow(hwnd, mem, PW_CLIENTONLY | PW_RENDERFULLCONTENT) != 0;
        SelectObject(mem, old);

        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(bi);
        bi.biWidth = w;
        bi.biHeight = h;  // bottom-up, as BMP files store it
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        const int stride = (w * 3 + 3) & ~3;
        std::vector<unsigned char> pixels(static_cast<size_t>(stride) * h);
        ok = ok && GetDIBits(mem, bmp, 0, h, pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS) == h;
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        if (!ok) return false;

        BITMAPFILEHEADER bf{};
        bf.bfType = 0x4D42;  // "BM"
        bf.bfOffBits = sizeof(bf) + sizeof(bi);
        bf.bfSize = bf.bfOffBits + static_cast<DWORD>(pixels.size());
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) return false;
        fwrite(&bf, sizeof(bf), 1, f);
        fwrite(&bi, sizeof(bi), 1, f);
        fwrite(pixels.data(), 1, pixels.size(), f);
        fclose(f);
        return true;
    }
#endif
}

void wg3d::capture::init() {
    const char* spec = std::getenv("WG3D_CAPTURE");
    if (!spec) return;
    if (const char* dir = std::getenv("WG3D_CAPTURE_DIR")) {
        out_dir = dir;
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
    }
    std::stringstream entries{ spec };
    std::string entry;
    while (std::getline(entries, entry, ',')) {
        size_t colon = entry.find(':');
        if (entry.empty() || colon == std::string::npos) continue;
        captures.push_back({ std::atof(entry.substr(0, colon).c_str()), entry.substr(colon + 1) });
    }
    std::sort(captures.begin(), captures.end(), [](const Capture& a, const Capture& b) { return a.time < b.time; });
    if (!captures.empty()) next_capture = captures.front().time;
    std::fprintf(stderr, "[wg3d] capture: %zu screenshots -> %s\n", captures.size(), out_dir.string().c_str());
}

void wg3d::capture::on_vi() {
    if (!render_only_for_captures || wg3d::input::script_time() < next_capture.load()) return;
    // Hold the game on this VI (the VI thread is blocked, so no new VI and nothing moves) while the frame
    // reaches the screen and the main thread captures it: the capture is exactly at its game time.
    std::this_thread::sleep_for(std::chrono::milliseconds(PresentWaitMs));
    std::unique_lock lock{ hold_mutex };
    holding = true;
    hold_cv.wait_for(lock, std::chrono::seconds(5), [] { return !holding; });
    holding = false;
}

void wg3d::capture::update(SDL_Window* window) {
    if (captures.empty() || window == nullptr) return;
    // Fast runs capture only while on_vi holds the game.
    std::unique_lock lock{ hold_mutex, std::defer_lock };
    if (render_only_for_captures) {
        lock.lock();
        if (!holding) return;
    }
    const double now = wg3d::input::script_time();
    for (Capture& c : captures) {
        if (c.done || now < c.time) continue;
        c.done = true;
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        bool ok = SDL_GetWindowWMInfo(window, &info) &&
                  save_bmp(info.info.win.window, out_dir / (c.label + ".bmp"));
        std::fprintf(stderr, "[wg3d] capture: %s at %.2fs (game frame %llu)%s\n", c.label.c_str(), now,
                     (unsigned long long)wg3d::stats::display_lists.load(), ok ? "" : " FAILED");
#endif
    }
    auto pending = std::find_if(captures.begin(), captures.end(), [](const Capture& c) { return !c.done; });
    next_capture = pending == captures.end() ? std::numeric_limits<double>::infinity() : pending->time;
    if (lock.owns_lock()) {
        holding = false;
        hold_cv.notify_all();
    }
}

void wg3d::capture::set_render_only_for_captures(bool enabled) {
    render_only_for_captures = enabled && !captures.empty();
    if (render_only_for_captures) {
        std::fprintf(stderr, "[wg3d] capture: rendering only %.1fs before each capture\n", RenderLead);
    }
}

bool wg3d::capture::render_needed() {
    return !render_only_for_captures || wg3d::input::script_time() >= next_capture.load() - RenderLead;
}
