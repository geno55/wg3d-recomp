#ifndef WG3D_INPUT_H
#define WG3D_INPUT_H

#include <array>
#include <cstdint>
#include <string>

#include <json/json.hpp>

#include "ultramodern/input.hpp"

union SDL_Event;

namespace wg3d::input {
    constexpr int NumPorts = 4;

    // N64 controller button bits (OSContPad.button).
    enum Button : uint16_t {
        A = 0x8000, B = 0x4000, Z = 0x2000, Start = 0x1000,
        DUp = 0x0800, DDown = 0x0400, DLeft = 0x0200, DRight = 0x0100,
        L = 0x0020, R = 0x0010,
        CUp = 0x0008, CDown = 0x0004, CLeft = 0x0002, CRight = 0x0001,
    };

    // Main thread: SDL init (after SDL video is up) and per-event / per-frame hooks.
    void init();
    void handle_event(const SDL_Event& event);
    void update();  // snapshots all ports; called after the event pump
    // Game time in seconds (VIs since the game started / 60): the clock for WG3D_INPUT_SCRIPT, WG3D_CAPTURE
    // and WG3D_FRAME_LOG. Independent of --speed and of host load.
    double script_time();
    // ultramodern's per-VI callback (events_callbacks.vi_callback, on the VI thread): advances script_time.
    void on_vi();

    // ultramodern input callbacks (called from game threads; read the snapshot).
    void poll_input();
    bool get_input(int port, uint16_t* buttons, float* x, float* y);
    void set_rumble(int port, bool rumble);
    ultramodern::input::connected_device_info_t get_connected_device_info(int port);

    // Settings menu (main thread). The N64 inputs that can be bound (A, B, ..., Stick-Right), in order.
    int num_targets();
    const char* target_name(int target);
    int max_bindings();  // per input and device
    // controls.json as in effect, and with every option at its default.
    nlohmann::json controls_settings();
    nlohmann::json default_controls_settings();
    // Applies new controls (same format as controls.json), puts the pads back on ports and writes the file.
    void set_controls_settings(const nlohmann::json& settings);
    // The controller on each port ("" if none).
    std::array<std::string, NumPorts> port_pad_names();
    // Re-reads which ports have a Controller Pak (general.json; WG3D_PAKS still wins).
    void refresh_paks();
    // While blocked (menu open), the game sees connected but idle controllers; after unblocking it keeps
    // seeing them idle until every button and stick has been released.
    void set_blocked(bool blocked);
}

#endif
