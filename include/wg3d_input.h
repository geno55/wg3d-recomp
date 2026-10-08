#ifndef WG3D_INPUT_H
#define WG3D_INPUT_H

#include <cstdint>

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
}

#endif
