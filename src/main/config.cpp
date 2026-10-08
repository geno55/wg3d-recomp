// Persistent settings (Phase 5 §1). Three librecomp configs, each a JSON file in the app folder
// (%APPDATA%\WG3DRecomp, or --data-dir):
//   general.json   Controller Pak per port
//   graphics.json  RT64 options (the ultramodern GraphicsConfig fields set_application_user_config maps)
//   window.json    last windowed size/position and maximised state (state, not settings: rewritten on exit)
//
// Defaults reproduce the pre-config behaviour. Missing or invalid values fall back to the default, and
// each file is rewritten after loading so it always lists every option.
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>

#include "SDL.h"

#include "librecomp/config.hpp"
#include "ultramodern/config.hpp"

#include "wg3d_config.h"
#include "wg3d_input.h"

namespace {
    using recomp::config::Config;
    using namespace ultramodern::renderer;

    constexpr int DefaultWidth = 1280;
    constexpr int DefaultHeight = 960;
    constexpr int MinWindowSize = 320;
    constexpr int MaxWindowSize = 16384;

    Config general{ "General", "general" };
    Config graphics{ "Graphics", "graphics" };
    Config window{ "Window", "window" };

    std::mutex window_mutex;
    wg3d::config::WindowState window_now{ false, 0, 0, DefaultWidth, DefaultHeight, false };
    bool fullscreen = false;

    std::string pak_option(int port) {
        return "pak_port_" + std::to_string(port + 1);
    }

    template <typename T>
    T get_enum(const Config& c, const std::string& id) {
        return static_cast<T>(std::get<uint32_t>(c.get_option_value(id)));
    }

    double get_number(const Config& c, const std::string& id, double min, double max) {
        return std::clamp(std::get<double>(c.get_option_value(id)), min, max);
    }

    bool get_bool(const Config& c, const std::string& id) {
        return std::get<bool>(c.get_option_value(id));
    }

    void define_general() {
        for (int port = 0; port < wg3d::input::NumPorts; port++) {
            general.add_bool_option(pak_option(port), "Controller Pak in port " + std::to_string(port + 1),
                                    "Saves go to saves/controllerpak_port<N>/ in the app folder.", port == 0);
        }
    }

    void define_graphics() {
        graphics.add_enum_option("resolution", "Resolution",
            "Original: 240p. Original2x: 480p. Auto: largest integer multiple of 240p that fits the window.",
            { { Resolution::Original, "Original" }, { Resolution::Original2x, "Original2x" }, { Resolution::Auto, "Auto" } },
            Resolution::Original);
        graphics.add_number_option("downsampling", "Downsampling",
            "Renders at N times the resolution and scales down (supersampling). Original/Original2x only.",
            1, 4, 1, 0, false, 1);
        graphics.add_enum_option("window_mode", "Window mode", "",
            { { WindowMode::Windowed, "Windowed" }, { WindowMode::Fullscreen, "Fullscreen" } },
            WindowMode::Windowed);
        graphics.add_enum_option("aspect_ratio", "Aspect ratio",
            "Expand renders the game wider than 4:3. Untested; may show culling and HUD problems.",
            { { AspectRatio::Original, "Original" }, { AspectRatio::Expand, "Expand" } },
            AspectRatio::Original);
        graphics.add_enum_option("hud_ratio", "HUD aspect ratio", "",
            { { HUDRatioMode::Original, "Original" }, { HUDRatioMode::Clamp16x9, "Clamp16x9" }, { HUDRatioMode::Full, "Full" } },
            HUDRatioMode::Original);
        graphics.add_enum_option("antialiasing", "Antialiasing", "",
            { { Antialiasing::None, "None" }, { Antialiasing::MSAA2X, "MSAA2X" }, { Antialiasing::MSAA4X, "MSAA4X" },
              { Antialiasing::MSAA8X, "MSAA8X" } },
            Antialiasing::None);
        graphics.add_enum_option("refresh_rate", "Refresh rate",
            "Original: the game's rate. Display: the monitor's. Manual: refresh_rate_manual.",
            { { RefreshRate::Original, "Original" }, { RefreshRate::Display, "Display" }, { RefreshRate::Manual, "Manual" } },
            RefreshRate::Original);
        graphics.add_number_option("refresh_rate_manual", "Manual refresh rate", "", 20, 360, 1, 0, false, 60);
        graphics.add_enum_option("high_precision_framebuffer", "High precision framebuffer", "",
            { { HighPrecisionFramebuffer::Auto, "Auto" }, { HighPrecisionFramebuffer::On, "On" },
              { HighPrecisionFramebuffer::Off, "Off" } },
            HighPrecisionFramebuffer::Auto);
        graphics.add_enum_option("graphics_api", "Graphics API", "Takes effect on the next launch.",
            { { GraphicsApi::Auto, "Auto" }, { GraphicsApi::D3D12, "D3D12" }, { GraphicsApi::Vulkan, "Vulkan" } },
            GraphicsApi::Auto);
    }

    void define_window() {
        window.add_bool_option("has_position", "", "", false, true);
        window.add_number_option("x", "", "", -MaxWindowSize, MaxWindowSize, 1, 0, false, 0, true);
        window.add_number_option("y", "", "", -MaxWindowSize, MaxWindowSize, 1, 0, false, 0, true);
        window.add_number_option("width", "", "", MinWindowSize, MaxWindowSize, 1, 0, false, DefaultWidth, true);
        window.add_number_option("height", "", "", MinWindowSize, MaxWindowSize, 1, 0, false, DefaultHeight, true);
        window.add_bool_option("maximized", "", "", false, true);
    }

    void apply_graphics() {
        GraphicsConfig g{};
        g.developer_mode = false;
        g.res_option = get_enum<Resolution>(graphics, "resolution");
        g.ds_option = (int)get_number(graphics, "downsampling", 1, 4);
        g.wm_option = get_enum<WindowMode>(graphics, "window_mode");
        g.ar_option = get_enum<AspectRatio>(graphics, "aspect_ratio");
        g.hr_option = get_enum<HUDRatioMode>(graphics, "hud_ratio");
        g.msaa_option = get_enum<Antialiasing>(graphics, "antialiasing");
        g.rr_option = get_enum<RefreshRate>(graphics, "refresh_rate");
        g.rr_manual_value = (int)get_number(graphics, "refresh_rate_manual", 20, 360);
        g.hpfb_option = get_enum<HighPrecisionFramebuffer>(graphics, "high_precision_framebuffer");
        g.api_option = get_enum<GraphicsApi>(graphics, "graphics_api");
        fullscreen = g.wm_option == WindowMode::Fullscreen;
        set_graphics_config(g);
    }

    // Loads one file (librecomp falls back to its .bak copy if the file is corrupt) and rewrites it with
    // every option present and numbers clamped to their range, so the file shows what is in effect.
    nlohmann::json load_one(Config& c) {
        if (!c.load_config()) {
            std::fprintf(stderr, "[wg3d] config: %s.json could not be loaded, using defaults\n", c.id.c_str());
        }
        nlohmann::json j = c.get_json_config();
        for (const auto& option : c.get_config_schema().options) {
            if (option.type == recomp::config::ConfigOptionType::Number) {
                const auto& n = std::get<recomp::config::ConfigOptionNumber>(option.variant);
                j[option.id] = (int)std::clamp(std::get<double>(c.get_option_value(option.id)), n.min, n.max);
            }
        }
        c.save_config_json(j);
        return j;
    }

    // A saved position is used only if the middle of the window's top edge is on a connected display,
    // so the title bar can be grabbed (monitors may have been unplugged since).
    bool position_visible(const wg3d::config::WindowState& s) {
        SDL_Point top{ s.x + s.width / 2, s.y + 8 };
        for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
            SDL_Rect bounds;
            if (SDL_GetDisplayUsableBounds(i, &bounds) == 0 && SDL_PointInRect(&top, &bounds)) {
                return true;
            }
        }
        return false;
    }
}

void wg3d::config::load() {
    define_general();
    define_graphics();
    define_window();
    load_one(general);
    nlohmann::json graphics_json = load_one(graphics);
    load_one(window);
    apply_graphics();
    std::fprintf(stderr, "[wg3d] config: graphics %s\n", graphics_json.dump().c_str());

    std::lock_guard lock{ window_mutex };
    window_now.has_position = get_bool(window, "has_position");
    window_now.x = (int)get_number(window, "x", -MaxWindowSize, MaxWindowSize);
    window_now.y = (int)get_number(window, "y", -MaxWindowSize, MaxWindowSize);
    window_now.width = (int)get_number(window, "width", MinWindowSize, MaxWindowSize);
    window_now.height = (int)get_number(window, "height", MinWindowSize, MaxWindowSize);
    window_now.maximized = get_bool(window, "maximized");
}

bool wg3d::config::pak_inserted(int port) {
    return port >= 0 && port < wg3d::input::NumPorts && get_bool(general, pak_option(port));
}

wg3d::config::WindowState wg3d::config::window_state() {
    std::lock_guard lock{ window_mutex };
    WindowState s = window_now;
    if (s.has_position && !position_visible(s)) {
        std::fprintf(stderr, "[wg3d] config: saved window position (%d, %d) is off-screen, centring\n", s.x, s.y);
        s.has_position = false;
    }
    return s;
}

void wg3d::config::track_window(SDL_Window* w) {
    if (w == nullptr || fullscreen) {
        return;
    }
    uint32_t flags = SDL_GetWindowFlags(w);
    if (flags & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_MINIMIZED)) {
        return;
    }
    std::lock_guard lock{ window_mutex };
    window_now.maximized = (flags & SDL_WINDOW_MAXIMIZED) != 0;
    if (!window_now.maximized) {
        // Keep the restored (un-maximised) placement, so un-maximising next session goes back to it.
        SDL_GetWindowPosition(w, &window_now.x, &window_now.y);
        SDL_GetWindowSize(w, &window_now.width, &window_now.height);
        window_now.has_position = true;
    }
}

void wg3d::config::save_window_state() {
    WindowState s;
    {
        std::lock_guard lock{ window_mutex };
        s = window_now;
    }
    // Written as JSON rather than through update_option_value: Config only stores values it read from
    // a file, so on a first run (no window.json yet) update_option_value would be a no-op.
    nlohmann::json j = window.get_json_config();
    j["has_position"] = s.has_position;
    j["x"] = s.x;
    j["y"] = s.y;
    j["width"] = std::clamp(s.width, MinWindowSize, MaxWindowSize);
    j["height"] = std::clamp(s.height, MinWindowSize, MaxWindowSize);
    j["maximized"] = s.maximized;
    if (!window.save_config_json(j)) {
        std::fprintf(stderr, "[wg3d] config: failed to write window.json\n");
    }
}
