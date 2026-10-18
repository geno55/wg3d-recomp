#pragma once
// The launcher and the settings menu (src/main/ui.cpp), drawn with Dear ImGui over RT64's output
// (src/main/ui_render.cpp).
#include <functional>
#include <mutex>
#include <string>

struct SDL_Window;
union SDL_Event;

namespace wg3d::ui {
    struct LauncherCallbacks {
        std::function<bool()> rom_ready;           // a valid ROM is stored
        std::function<std::string()> choose_rom;   // file picker; "" on success, else a message for the player
        std::function<void()> start_game;
        std::string message;                       // shown at first (e.g. why the command-line ROM was refused)
    };

    // Before the renderer is created: registers RT64's render hooks.
    void install_render_hooks();
    // Main thread, once the window exists. With a launcher, the UI starts on the launcher screen and the
    // game starts from it; without one (test runs) the game is expected to start right away.
    void init(SDL_Window* window, const LauncherCallbacks* launcher);
    // Main thread: every SDL event, then update() once per event pump.
    void handle_event(const SDL_Event& event);
    void update();

    // True while the launcher or the menu is on screen (the renderer then presents every VI).
    bool visible();

    // ui_render.cpp <-> ui.cpp: ImGui state is touched under this mutex; frames are built only once
    // the render backend exists.
    std::mutex& frame_mutex();
    bool backend_ready();
}
