// In-process screenshots for the scenario harness (chunk 4.1).
//
// WG3D_CAPTURE="t1:label1,t2:label2,..." saves the window's client area at script time t (seconds on
// the WG3D_INPUT_SCRIPT clock, wg3d::input::script_time) to WG3D_CAPTURE_DIR/<label>.bmp (default: the
// working directory). Captures run on the main thread right after the event pump, so they are aligned
// with scripted input to within one main-loop iteration, unlike an external capture.
//
// PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT) reads the DWM-composed window, so it works while
// other windows cover it.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

#include "SDL.h"
#include "SDL_syswm.h"

#include "wg3d_capture.h"
#include "wg3d_input.h"

namespace {
    struct Capture {
        double time;
        std::string label;
        bool done = false;
    };
    std::vector<Capture> captures;
    std::filesystem::path out_dir = ".";

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
    std::fprintf(stderr, "[wg3d] capture: %zu screenshots -> %s\n", captures.size(), out_dir.string().c_str());
}

void wg3d::capture::update(SDL_Window* window) {
    if (captures.empty() || window == nullptr) return;
    const double now = wg3d::input::script_time();
    for (Capture& c : captures) {
        if (c.done || now < c.time) continue;
        c.done = true;
#ifdef _WIN32
        SDL_SysWMinfo info;
        SDL_VERSION(&info.version);
        bool ok = SDL_GetWindowWMInfo(window, &info) &&
                  save_bmp(info.info.win.window, out_dir / (c.label + ".bmp"));
        std::fprintf(stderr, "[wg3d] capture: %s at %.2fs%s\n", c.label.c_str(), now, ok ? "" : " FAILED");
#endif
    }
}
