// Controller input (chunk 3.5): SDL2 game controllers on N64 ports 1-4 plus a keyboard map on port 1.
//
// Threading: SDL state is read only on the main thread (update(), right after the event pump);
// the ultramodern callbacks, called from game threads, read a mutex-protected snapshot.
//
// Ports: game controllers take the lowest free port in connection order (hot-plug supported; the
// game sees a newly connected pad the next time it queries the controllers). Port 1 always reports
// a controller, because the keyboard is merged into it.
//
// Default gamepad map (Xbox layout; remapping UI is Phase 5):
//   left stick / A / X / Start / D-pad -> stick / A / B / Start / D-pad
//   right stick -> C buttons (also Y = C-Left, B = C-Down)
//   LT -> Z, RT and RB -> R, LB -> L
// Keyboard map (port 1):
//   arrows or WASD -> stick, X -> A, C -> B, Z or Left Shift -> Z, Enter -> Start,
//   Q -> L, E -> R, I/J/K/L -> C up/left/down/right, T/F/G/H -> D-pad up/left/down/right
//
// Testing without a pad: WG3D_INPUT_SCRIPT="t:spec[:dur];..." presses `spec` on port 1 at t seconds
// after init for dur seconds (default 0.15). spec is '+'-joined button names (A B Z START L R DU DD DL
// DR CU CD CL CR) and/or X=<f> / Y=<f> stick values, e.g. "12:START;14.5:A;16:Y=-1:0.5".
// WG3D_INPUT_LOG=1 logs every change of the buttons/stick the game receives.
// WG3D_FAKE_PADS=N reports ports 2..N as connected idle controllers (port-detection tests, 3.7 gate).
// WG3D_PAKS="1,2" sets which ports have a Controller Pak inserted, overriding general.json
// (pak_port_N; default port 1 only). "none" for no paks.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "SDL.h"

#include "wg3d_config.h"
#include "wg3d_input.h"

namespace {
    using namespace wg3d::input;

    struct PortState {
        bool connected = false;
        uint16_t buttons = 0;
        float x = 0.0f;
        float y = 0.0f;
    };

    struct ScriptEvent {
        double start, end;
        uint16_t buttons;
        bool has_x, has_y;
        float x, y;
    };

    constexpr float StickDeadzone = 0.15f;   // radial, on the normalised stick
    constexpr float CStickThreshold = 0.5f;  // right stick -> C button
    constexpr int TriggerThreshold = 8000;   // of 32767

    std::array<SDL_GameController*, NumPorts> pads{};
    std::array<PortState, NumPorts> snapshot{};
    std::mutex snapshot_mutex;
    std::vector<ScriptEvent> script;
    std::chrono::steady_clock::time_point start_time;
    bool log_input = false;
    int fake_pads = 0;
    std::array<bool, NumPorts> pak_inserted{};  // general.json, then WG3D_PAKS

    float axis(SDL_GameController* pad, SDL_GameControllerAxis a) {
        return std::clamp(SDL_GameControllerGetAxis(pad, a) / 32767.0f, -1.0f, 1.0f);
    }

    // Radial deadzone with rescaling, so small stick noise reads as centred and full tilt stays 1.0.
    void apply_deadzone(float& x, float& y) {
        float mag = std::sqrt(x * x + y * y);
        if (mag < StickDeadzone) {
            x = y = 0.0f;
            return;
        }
        float scaled = std::min(1.0f, (mag - StickDeadzone) / (1.0f - StickDeadzone));
        x = x / mag * scaled;
        y = y / mag * scaled;
    }

    PortState read_pad(SDL_GameController* pad) {
        PortState s{ true };
        auto btn = [pad](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
        if (btn(SDL_CONTROLLER_BUTTON_A)) s.buttons |= A;
        if (btn(SDL_CONTROLLER_BUTTON_X)) s.buttons |= B;
        if (btn(SDL_CONTROLLER_BUTTON_Y)) s.buttons |= CLeft;
        if (btn(SDL_CONTROLLER_BUTTON_B)) s.buttons |= CDown;
        if (btn(SDL_CONTROLLER_BUTTON_START)) s.buttons |= Start;
        if (btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) s.buttons |= L;
        if (btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) s.buttons |= R;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_UP)) s.buttons |= DUp;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) s.buttons |= DDown;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) s.buttons |= DLeft;
        if (btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) s.buttons |= DRight;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > TriggerThreshold) s.buttons |= Z;
        if (SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > TriggerThreshold) s.buttons |= R;
        float rx = axis(pad, SDL_CONTROLLER_AXIS_RIGHTX), ry = axis(pad, SDL_CONTROLLER_AXIS_RIGHTY);
        if (rx < -CStickThreshold) s.buttons |= CLeft;
        if (rx > CStickThreshold) s.buttons |= CRight;
        if (ry < -CStickThreshold) s.buttons |= CUp;
        if (ry > CStickThreshold) s.buttons |= CDown;
        s.x = axis(pad, SDL_CONTROLLER_AXIS_LEFTX);
        s.y = -axis(pad, SDL_CONTROLLER_AXIS_LEFTY);  // SDL: down is positive; N64: up is positive
        apply_deadzone(s.x, s.y);
        return s;
    }

    void read_keyboard(PortState& s) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        struct { SDL_Scancode key; uint16_t button; } map[] = {
            { SDL_SCANCODE_X, A }, { SDL_SCANCODE_C, B }, { SDL_SCANCODE_Z, Z }, { SDL_SCANCODE_LSHIFT, Z },
            { SDL_SCANCODE_RETURN, Start }, { SDL_SCANCODE_Q, L }, { SDL_SCANCODE_E, R },
            { SDL_SCANCODE_I, CUp }, { SDL_SCANCODE_K, CDown }, { SDL_SCANCODE_J, CLeft }, { SDL_SCANCODE_L, CRight },
            { SDL_SCANCODE_T, DUp }, { SDL_SCANCODE_G, DDown }, { SDL_SCANCODE_F, DLeft }, { SDL_SCANCODE_H, DRight },
        };
        for (auto& m : map) {
            if (k[m.key]) s.buttons |= m.button;
        }
        float kx = float((k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) - (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]));
        float ky = float((k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) - (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]));
        if (kx != 0.0f || ky != 0.0f) {
            float mag = std::sqrt(kx * kx + ky * ky);
            s.x = kx / mag;
            s.y = ky / mag;
        }
    }

    uint16_t button_from_name(const std::string& n) {
        static const struct { const char* name; uint16_t bit; } names[] = {
            { "A", A }, { "B", B }, { "Z", Z }, { "START", Start }, { "L", L }, { "R", R },
            { "DU", DUp }, { "DD", DDown }, { "DL", DLeft }, { "DR", DRight },
            { "CU", CUp }, { "CD", CDown }, { "CL", CLeft }, { "CR", CRight },
        };
        for (auto& e : names) {
            if (n == e.name) return e.bit;
        }
        std::fprintf(stderr, "[wg3d] input script: unknown button '%s'\n", n.c_str());
        return 0;
    }

    void parse_script(const char* text) {
        std::stringstream entries{ text };
        std::string entry;
        while (std::getline(entries, entry, ';')) {
            if (entry.empty()) continue;
            std::stringstream fields{ entry };
            std::string t, spec, dur;
            std::getline(fields, t, ':');
            std::getline(fields, spec, ':');
            std::getline(fields, dur, ':');
            ScriptEvent ev{};
            ev.start = std::atof(t.c_str());
            ev.end = ev.start + (dur.empty() ? 0.15 : std::atof(dur.c_str()));
            std::stringstream parts{ spec };
            std::string part;
            while (std::getline(parts, part, '+')) {
                if (part.rfind("X=", 0) == 0) { ev.has_x = true; ev.x = (float)std::atof(part.c_str() + 2); }
                else if (part.rfind("Y=", 0) == 0) { ev.has_y = true; ev.y = (float)std::atof(part.c_str() + 2); }
                else ev.buttons |= button_from_name(part);
            }
            script.push_back(ev);
        }
        std::fprintf(stderr, "[wg3d] input script: %zu events\n", script.size());
    }

    void apply_script(PortState& s) {
        double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        for (const ScriptEvent& ev : script) {
            if (t >= ev.start && t < ev.end) {
                s.buttons |= ev.buttons;
                if (ev.has_x) s.x = ev.x;
                if (ev.has_y) s.y = ev.y;
            }
        }
    }

    void open_pad(int device_index) {
        SDL_GameController* pad = SDL_GameControllerOpen(device_index);
        if (!pad) return;
        SDL_JoystickID id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
        for (SDL_GameController* p : pads) {
            if (p && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(p)) == id) {
                SDL_GameControllerClose(pad);  // already open (duplicate ADDED event)
                return;
            }
        }
        for (int port = 0; port < NumPorts; port++) {
            if (!pads[port]) {
                pads[port] = pad;
                std::fprintf(stderr, "[wg3d] input: \"%s\" -> port %d\n", SDL_GameControllerName(pad), port + 1);
                return;
            }
        }
        std::fprintf(stderr, "[wg3d] input: \"%s\" ignored (all 4 ports in use)\n", SDL_GameControllerName(pad));
        SDL_GameControllerClose(pad);
    }

    void close_pad(SDL_JoystickID id) {
        for (int port = 0; port < NumPorts; port++) {
            if (pads[port] && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[port])) == id) {
                std::fprintf(stderr, "[wg3d] input: port %d disconnected\n", port + 1);
                SDL_GameControllerClose(pads[port]);
                pads[port] = nullptr;
            }
        }
    }
}

void wg3d::input::init() {
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
    start_time = std::chrono::steady_clock::now();
    log_input = std::getenv("WG3D_INPUT_LOG") != nullptr;
    if (const char* n = std::getenv("WG3D_FAKE_PADS")) {
        fake_pads = std::clamp(std::atoi(n), 0, NumPorts);
    }
    if (const char* s = std::getenv("WG3D_INPUT_SCRIPT")) {
        parse_script(s);
    }
    for (int port = 0; port < NumPorts; port++) {
        pak_inserted[port] = wg3d::config::pak_inserted(port);
    }
    if (const char* p = std::getenv("WG3D_PAKS")) {
        // Port numbers with an inserted pak, e.g. "1", "1,2" or "none".
        pak_inserted = {};
        for (const char* c = p; *c; c++) {
            if (*c >= '1' && *c <= '0' + NumPorts) pak_inserted[*c - '1'] = true;
        }
    }
    std::fprintf(stderr, "[wg3d] input: controller paks in ports:%s%s%s%s\n", pak_inserted[0] ? " 1" : "",
                 pak_inserted[1] ? " 2" : "", pak_inserted[2] ? " 3" : "", pak_inserted[3] ? " 4" : "");
    // Pads present at start-up also arrive as SDL_CONTROLLERDEVICEADDED events; open_pad ignores
    // duplicates, so opening them here just makes them available before the game's first query.
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) open_pad(i);
    }
    update();
}

void wg3d::input::handle_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERDEVICEADDED) {
        open_pad(event.cdevice.which);
    } else if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
        close_pad(event.cdevice.which);
    }
}

void wg3d::input::update() {
    std::array<PortState, NumPorts> next{};
    for (int port = 0; port < NumPorts; port++) {
        if (pads[port]) next[port] = read_pad(pads[port]);
    }
    for (int port = 1; port < fake_pads; port++) next[port].connected = true;
    next[0].connected = true;  // keyboard
    read_keyboard(next[0]);
    apply_script(next[0]);

    std::lock_guard lock{ snapshot_mutex };
    if (log_input) {
        for (int port = 0; port < NumPorts; port++) {
            const PortState &a = snapshot[port], &b = next[port];
            if (a.connected != b.connected || a.buttons != b.buttons || a.x != b.x || a.y != b.y) {
                std::fprintf(stderr, "[wg3d] input: port %d %s buttons=%04X stick=(%.2f, %.2f)\n", port + 1,
                             b.connected ? "connected" : "absent", b.buttons, b.x, b.y);
            }
        }
    }
    snapshot = next;
}

void wg3d::input::poll_input() {}

bool wg3d::input::get_input(int port, uint16_t* buttons, float* x, float* y) {
    if (port < 0 || port >= NumPorts) return false;
    std::lock_guard lock{ snapshot_mutex };
    const PortState& s = snapshot[port];
    if (!s.connected) return false;
    *buttons = s.buttons;
    *x = s.x;
    *y = s.y;
    return true;
}

// The US release has no Rumble Pak support.
void wg3d::input::set_rumble(int, bool) {}

ultramodern::input::connected_device_info_t wg3d::input::get_connected_device_info(int port) {
    bool connected = false;
    if (port >= 0 && port < NumPorts) {
        std::lock_guard lock{ snapshot_mutex };
        connected = snapshot[port].connected;
    }
    if (!connected) {
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    // Controller Pak (chunk 3.6): the runtime's osPfs* store each port's pak under
    // %APPDATA%/WG3DRecomp/saves/controllerpak_port<N>/.
    return { ultramodern::input::Device::Controller,
             pak_inserted[port] ? ultramodern::input::Pak::ControllerPak : ultramodern::input::Pak::None };
}
