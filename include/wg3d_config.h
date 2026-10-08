#pragma once
// Persistent settings (Phase 5 §1): general.json, graphics.json and window.json in the app folder,
// stored through librecomp's recomp::config::Config.

struct SDL_Window;

namespace wg3d::config {
    struct WindowState {
        bool has_position;  // false until a windowed position has been saved; then x/y are used
        int x, y;
        int width, height;
        bool maximized;
    };

    // Loads (or creates) the config files and applies the graphics settings to ultramodern. Call after
    // recomp::register_config_path and before recomp::start.
    void load();

    // Controller Pak inserted in this port (0-3), per general.json. WG3D_PAKS overrides it in input.cpp.
    bool pak_inserted(int port);

    // Window placement to restore, clamped so the window lands on a connected display.
    WindowState window_state();
    // Records the window's current placement (on SDL window events). Ignored while fullscreen or minimised.
    void track_window(SDL_Window* window);
    // Writes window.json. Call once on exit.
    void save_window_state();
}
