#pragma once
// Persistent settings: general.json, graphics.json, sound.json and window.json in the app folder,
// stored through librecomp's recomp::config::Config. (controls.json: src/game/input.cpp.)
#include <string>

#include "librecomp/config.hpp"

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

    struct SoundSettings {
        int master_volume;         // 0-100
        bool mute_when_unfocused;
    };
    SoundSettings sound_settings();

    // Controller Pak inserted in this port (0-3), per general.json. WG3D_PAKS overrides it in input.cpp.
    bool pak_inserted(int port);
    bool pause_when_unfocused();
    // graphics.json aspect_ratio is Expand (widescreen game patches, src/game/widescreen.cpp).
    bool widescreen();

    // The files the settings menu edits (window.json is state, not settings).
    enum class Section { General, Graphics, Sound };
    // A section's options (schema); read their values with get_value.
    const recomp::config::Config& section_config(Section section);
    recomp::config::ConfigValueVariant get_value(Section section, const std::string& id);
    // Sets an option, rewrites its file and applies it: graphics go to the renderer, paks to the ports;
    // sound is read on the audio path.
    void set_value(Section section, const std::string& id, const recomp::config::ConfigValueVariant& value);

    // Window placement to restore, clamped so the window lands on a connected display.
    WindowState window_state();
    // Records the window's current placement (on SDL window events). Ignored while fullscreen or minimised.
    void track_window(SDL_Window* window);
    // Switches between windowed and borderless fullscreen, and saves window_mode to graphics.json
    // (Alt+Enter / F11).
    void toggle_fullscreen();
    // Writes window.json. Call once on exit.
    void save_window_state();
}
