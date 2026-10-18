// Controller input: SDL2 game controllers on N64 ports 1-4 plus the keyboard on one port.
//
// Threading: SDL state is read only on the main thread (update(), right after the event pump);
// the ultramodern callbacks, called from game threads, read a mutex-protected snapshot.
//
// controls.json in the app folder (written with the defaults on first run, rewritten on load so it
// lists every option; the settings menu edits it through set_controls_settings, or edit it while the game
// is closed):
//   keyboard_port  1-4: the port the keyboard drives (merged with a pad on that port); 0: keyboard off
//   deadzone       percent of the gamepad stick's travel that reads as centred (radial, rescaled)
//   pads           per port, part of a controller's name (case-insensitive) to put on that port; "" =
//                  the next free port. Unreserved pads take the lowest free unreserved port in connection
//                  order (hot-plug supported: the game sees a new pad the next time it queries).
//   keyboard       N64 input -> up to two SDL key names ("X", "Left Shift", "Return", "Up", ...)
//   gamepad        N64 input -> up to two SDL controller inputs: button names ("a", "x", "start",
//                  "leftshoulder", "dpup", ...) or an axis and direction ("leftx-", "righty+",
//                  "lefttrigger"). One map for every pad.
// N64 inputs: A B Z Start L R C-Up C-Down C-Left C-Right D-Up D-Down D-Left D-Right and Stick-Up /
// Stick-Down / Stick-Left / Stick-Right. A stick direction bound to an axis is analog; one bound to a
// key or button gives full tilt. ultramodern maps the stick onto the N64's octagonal range.
// Defaults (Xbox layout): left stick -> stick, A/X -> A/B, LT -> Z, LB -> L, RB and RT -> R, right stick
// -> C buttons (also Y = C-Left, B = C-Down), D-pad -> D-pad. Keyboard: arrows or WASD -> stick,
// X -> A, C -> B, Z or Left Shift -> Z, Enter -> Start, Q -> L, E -> R, I/J/K/L -> C up/left/down/right,
// T/F/G/H -> D-pad up/left/down/right.
//
// Testing without a pad: WG3D_INPUT_SCRIPT="t:spec[:dur];..." presses `spec` on port 1 at t seconds
// for dur seconds (default 0.15). Script time is game time, VIs delivered since the game started / 60,
// so a script hits the same frames at any --speed and under host load. spec is '+'-joined button names (A B Z START L R DU DD DL
// DR CU CD CL CR) and/or X=<f> / Y=<f> stick values, e.g. "12:START;14.5:A;16:Y=-1:0.5". A "<n>>" prefix
// sends the event to port n instead (with WG3D_FAKE_PADS >= n), e.g. "15:2>START".
// WG3D_INPUT_LOG=1 logs every change of the buttons/stick the game receives.
// WG3D_FAKE_PADS=N reports ports 2..N as connected idle controllers (port-detection tests, boot gate).
// WG3D_PAKS="1,2" sets which ports have a Controller Pak inserted, overriding general.json
// (pak_port_N; default every port). "none" for no paks.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "SDL.h"
#include <json/json.hpp>

#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"

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
        int port;
        double start, end;
        uint16_t buttons;
        bool has_x, has_y;
        float x, y;
    };

    constexpr float AxisThreshold = 0.5f;      // a stick axis bound to a button
    constexpr float TriggerThreshold = 0.25f;  // a trigger bound to a button (8000 of 32767)
    constexpr int MaxBindings = 2;

    // The N64 inputs that can be bound, in controls.json order: the 14 buttons, then the stick directions.
    enum StickDir { StickUp, StickDown, StickLeft, StickRight, NumStickDirs };
    struct Target { const char* name; uint16_t button; int stick; };
    constexpr Target Targets[] = {
        { "A", A, -1 }, { "B", B, -1 }, { "Z", Z, -1 }, { "Start", Start, -1 }, { "L", L, -1 }, { "R", R, -1 },
        { "C-Up", CUp, -1 }, { "C-Down", CDown, -1 }, { "C-Left", CLeft, -1 }, { "C-Right", CRight, -1 },
        { "D-Up", DUp, -1 }, { "D-Down", DDown, -1 }, { "D-Left", DLeft, -1 }, { "D-Right", DRight, -1 },
        { "Stick-Up", 0, StickUp }, { "Stick-Down", 0, StickDown }, { "Stick-Left", 0, StickLeft },
        { "Stick-Right", 0, StickRight },
    };
    constexpr int NumTargets = sizeof(Targets) / sizeof(Targets[0]);

    struct DefaultBinding { const char* keys[MaxBindings]; const char* pad[MaxBindings]; };
    constexpr DefaultBinding Defaults[NumTargets] = {
        { { "X" }, { "a" } }, { { "C" }, { "x" } }, { { "Z", "Left Shift" }, { "lefttrigger" } },
        { { "Return" }, { "start" } }, { { "Q" }, { "leftshoulder" } }, { { "E" }, { "rightshoulder", "righttrigger" } },
        { { "I" }, { "righty-" } }, { { "K" }, { "righty+", "b" } }, { { "J" }, { "rightx-", "y" } },
        { { "L" }, { "rightx+" } }, { { "T" }, { "dpup" } }, { { "G" }, { "dpdown" } }, { { "F" }, { "dpleft" } },
        { { "H" }, { "dpright" } },
        { { "Up", "W" }, { "lefty-" } }, { { "Down", "S" }, { "lefty+" } }, { { "Left", "A" }, { "leftx-" } },
        { { "Right", "D" }, { "leftx+" } },
    };

    // A gamepad input: a button, or one direction of an axis (triggers only go positive).
    struct PadInput {
        bool is_axis;
        int index;  // SDL_GameControllerButton or SDL_GameControllerAxis
        int sign;   // axis: +1 or -1
    };

    struct Controls {
        int keyboard_port = 0;  // index, -1 = keyboard off
        float deadzone = 0.15f;
        std::array<std::string, NumPorts> pad_names{};  // lower case; "" = any
        std::vector<SDL_Scancode> keys[NumTargets];
        std::vector<PadInput> pad[NumTargets];
    };
    Controls controls;

    std::array<SDL_GameController*, NumPorts> pads{};
    std::array<PortState, NumPorts> snapshot{};
    std::mutex snapshot_mutex;
    std::vector<ScriptEvent> script;
    std::atomic<uint64_t> game_vis{ 0 };  // VIs since the game started (on_vi)
    std::chrono::steady_clock::time_point init_time;  // for the "game started" message only
    std::array<PortState, NumPorts> last_logged{};
    bool log_input = false;
    int fake_pads = 0;
    std::array<bool, NumPorts> pak_inserted{};  // general.json, or WG3D_PAKS; guarded by snapshot_mutex
    const char* paks_override = nullptr;        // WG3D_PAKS
    nlohmann::json controls_json;               // controls.json as last applied (canonical)
    bool blocked = false;                       // settings menu open: the game sees no presses
    bool suppressed = false;                    // after the menu: no presses until everything is released

    // ---------------------------------------------------------------------------------------------
    // controls.json
    // ---------------------------------------------------------------------------------------------
    bool parse_pad_input(const std::string& name, PadInput& out) {
        SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(name.c_str());
        if (b != SDL_CONTROLLER_BUTTON_INVALID) {
            out = { false, b, 0 };
            return true;
        }
        std::string axis_name = name;
        int sign = 1;
        if (!axis_name.empty() && (axis_name.back() == '+' || axis_name.back() == '-')) {
            sign = axis_name.back() == '-' ? -1 : 1;
            axis_name.pop_back();
        }
        SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(axis_name.c_str());
        if (a != SDL_CONTROLLER_AXIS_INVALID) {
            out = { true, a, sign };
            return true;
        }
        return false;
    }

    std::string pad_input_name(const PadInput& p) {
        if (!p.is_axis) return SDL_GameControllerGetStringForButton((SDL_GameControllerButton)p.index);
        std::string n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)p.index);
        bool trigger = p.index == SDL_CONTROLLER_AXIS_TRIGGERLEFT || p.index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        return trigger ? n : n + (p.sign < 0 ? "-" : "+");
    }

    // Applies controls (missing or invalid entries take the default) and returns them complete, in the
    // canonical spelling.
    nlohmann::json apply_controls(const nlohmann::json& in) {
        auto number = [&](const char* key, int def, int lo, int hi) {
            const auto it = in.find(key);
            return it != in.end() && it->is_number() ? std::clamp((int)it->get<double>(), lo, hi) : def;
        };
        int keyboard_port = number("keyboard_port", 1, 0, NumPorts);
        int deadzone = number("deadzone", 15, 0, 50);
        controls.keyboard_port = keyboard_port - 1;
        controls.deadzone = deadzone / 100.0f;

        nlohmann::json out = nlohmann::json::object();
        out["keyboard_port"] = keyboard_port;
        out["deadzone"] = deadzone;
        out["pads"] = nlohmann::json::array();
        const auto pads_in = in.find("pads");
        for (int port = 0; port < NumPorts; port++) {
            std::string name;
            if (pads_in != in.end() && pads_in->is_array() && port < (int)pads_in->size() && (*pads_in)[port].is_string()) {
                name = (*pads_in)[port].get<std::string>();
            }
            out["pads"].push_back(name);
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return (char)std::tolower(c); });
            controls.pad_names[port] = name;
        }

        // One binding list: the strings in the file, or the defaults if the entry is missing or not a list.
        auto names_for = [&](const char* section, int t, const char* const* defaults) {
            std::vector<std::string> names;
            const auto sec = in.find(section);
            const nlohmann::json* list = nullptr;
            if (sec != in.end() && sec->is_object()) {
                const auto it = sec->find(Targets[t].name);
                if (it != sec->end() && it->is_array()) list = &*it;
            }
            if (list) {
                for (const auto& v : *list) {
                    if (v.is_string()) names.push_back(v.get<std::string>());
                }
            } else {
                for (int i = 0; i < MaxBindings && defaults[i]; i++) names.push_back(defaults[i]);
            }
            if ((int)names.size() > MaxBindings) {
                std::fprintf(stderr, "[wg3d] controls: %s %s has more than %d bindings; using the first %d\n",
                             section, Targets[t].name, MaxBindings, MaxBindings);
                names.resize(MaxBindings);
            }
            return names;
        };
        out["keyboard"] = nlohmann::json::object();
        out["gamepad"] = nlohmann::json::object();
        for (int t = 0; t < NumTargets; t++) {
            controls.keys[t].clear();
            nlohmann::json keys = nlohmann::json::array();
            for (const std::string& n : names_for("keyboard", t, Defaults[t].keys)) {
                SDL_Scancode sc = SDL_GetScancodeFromName(n.c_str());
                if (sc == SDL_SCANCODE_UNKNOWN) {
                    std::fprintf(stderr, "[wg3d] controls: keyboard %s: unknown key \"%s\"\n", Targets[t].name, n.c_str());
                    continue;
                }
                controls.keys[t].push_back(sc);
                keys.push_back(SDL_GetScancodeName(sc));
            }
            out["keyboard"][Targets[t].name] = keys;

            controls.pad[t].clear();
            nlohmann::json pad = nlohmann::json::array();
            for (const std::string& n : names_for("gamepad", t, Defaults[t].pad)) {
                PadInput p;
                if (!parse_pad_input(n, p)) {
                    std::fprintf(stderr, "[wg3d] controls: gamepad %s: unknown input \"%s\"\n", Targets[t].name, n.c_str());
                    continue;
                }
                controls.pad[t].push_back(p);
                pad.push_back(pad_input_name(p));
            }
            out["gamepad"][Targets[t].name] = pad;
        }
        std::fprintf(stderr, "[wg3d] controls: keyboard on %s, deadzone %d%%\n",
                     keyboard_port ? ("port " + std::to_string(keyboard_port)).c_str() : "no port", deadzone);
        return out;
    }

    void write_controls(const nlohmann::json& out) {
        std::filesystem::path path = recomp::get_config_path() / "controls.json";
        // Written by hand rather than with dump(): options in a fixed order (nlohmann sorts object keys) and
        // one N64 input per line, in controller order, so the file is easy to edit.
        std::string text = "{\n";
        text += "    \"keyboard_port\": " + out["keyboard_port"].dump() + ",\n";
        text += "    \"deadzone\": " + out["deadzone"].dump() + ",\n";
        text += "    \"pads\": " + out["pads"].dump() + ",\n";
        for (const char* section : { "keyboard", "gamepad" }) {
            text += std::string("    \"") + section + "\": {\n";
            for (int t = 0; t < NumTargets; t++) {
                text += "        " + nlohmann::json(Targets[t].name).dump() + ": " + out[section][Targets[t].name].dump();
                text += t + 1 < NumTargets ? ",\n" : "\n";
            }
            text += std::string("    }") + (std::string(section) == "keyboard" ? ",\n" : "\n");
        }
        text += "}\n";
        if (std::ofstream f{ path }; f.good()) {
            f << text;
        } else {
            std::fprintf(stderr, "[wg3d] controls: could not write %s\n", path.string().c_str());
        }
    }

    // Reads controls.json and writes it back complete, so the file always shows what is in effect.
    void load_controls() {
        std::filesystem::path path = recomp::get_config_path() / "controls.json";
        nlohmann::json in = nlohmann::json::object();
        if (std::ifstream f{ path }; f.good()) {
            in = nlohmann::json::parse(f, nullptr, false);
            if (!in.is_object()) {
                std::fprintf(stderr, "[wg3d] controls: %s is not valid JSON, using the defaults\n", path.string().c_str());
                in = nlohmann::json::object();
            }
        }
        controls_json = apply_controls(in);
        write_controls(controls_json);
    }

    // ---------------------------------------------------------------------------------------------
    // Reading the devices
    // ---------------------------------------------------------------------------------------------
    // How far a pad input is pushed, 0..1 (buttons are 0 or 1; an axis only counts in its direction).
    float pad_value(SDL_GameController* pad, const PadInput& p) {
        if (!p.is_axis) return SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)p.index) ? 1.0f : 0.0f;
        float v = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)p.index) / 32767.0f;
        return std::clamp(v * p.sign, 0.0f, 1.0f);
    }

    bool pad_pressed(SDL_GameController* pad, const PadInput& p) {
        if (!p.is_axis) return pad_value(pad, p) > 0.0f;
        bool trigger = p.index == SDL_CONTROLLER_AXIS_TRIGGERLEFT || p.index == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        return pad_value(pad, p) > (trigger ? TriggerThreshold : AxisThreshold);
    }

    // Radial deadzone with rescaling, so small stick noise reads as centred and full tilt stays 1.0.
    void apply_deadzone(float& x, float& y) {
        float mag = std::sqrt(x * x + y * y);
        if (mag <= controls.deadzone || mag == 0.0f) {
            x = y = 0.0f;
            return;
        }
        float scaled = std::min(1.0f, (mag - controls.deadzone) / (1.0f - controls.deadzone));
        x = x / mag * scaled;
        y = y / mag * scaled;
    }

    PortState read_pad(SDL_GameController* pad) {
        PortState s{ true };
        float dir[NumStickDirs] = {};
        for (int t = 0; t < NumTargets; t++) {
            for (const PadInput& p : controls.pad[t]) {
                if (Targets[t].stick >= 0) {
                    dir[Targets[t].stick] = std::max(dir[Targets[t].stick], pad_value(pad, p));
                } else if (pad_pressed(pad, p)) {
                    s.buttons |= Targets[t].button;
                }
            }
        }
        s.x = dir[StickRight] - dir[StickLeft];
        s.y = dir[StickUp] - dir[StickDown];
        apply_deadzone(s.x, s.y);
        return s;
    }

    void read_keyboard(PortState& s) {
        const Uint8* k = SDL_GetKeyboardState(nullptr);
        // Alt+Enter toggles fullscreen (main.cpp), so Enter doesn't count while Alt is held.
        const bool alt = k[SDL_SCANCODE_LALT] || k[SDL_SCANCODE_RALT];
        bool dir[NumStickDirs] = {};
        for (int t = 0; t < NumTargets; t++) {
            for (SDL_Scancode sc : controls.keys[t]) {
                if (!k[sc] || (alt && sc == SDL_SCANCODE_RETURN)) continue;
                if (Targets[t].stick >= 0) {
                    dir[Targets[t].stick] = true;
                } else {
                    s.buttons |= Targets[t].button;
                }
            }
        }
        float kx = float(dir[StickRight] - dir[StickLeft]);
        float ky = float(dir[StickUp] - dir[StickDown]);
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
            if (spec.size() > 2 && spec[1] == '>' && spec[0] >= '1' && spec[0] <= '0' + NumPorts) {
                ev.port = spec[0] - '1';
                spec = spec.substr(2);
            }
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

    void apply_script(int port, PortState& s) {
        double t = script_time();
        for (const ScriptEvent& ev : script) {
            if (ev.port == port && t >= ev.start && t < ev.end) {
                s.buttons |= ev.buttons;
                if (ev.has_x) s.x = ev.x;
                if (ev.has_y) s.y = ev.y;
            }
        }
    }

    // Puts an open pad on a port: one reserved for it by name (controls.json "pads"), else the lowest free
    // unreserved port, else any free port. Closes it if all four are taken.
    void assign_pad(SDL_GameController* pad) {
        std::string name = SDL_GameControllerName(pad) ? SDL_GameControllerName(pad) : "";
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        int chosen = -1;
        for (int port = 0; port < NumPorts && chosen < 0; port++) {
            const std::string& want = controls.pad_names[port];
            if (!pads[port] && !want.empty() && lower.find(want) != std::string::npos) chosen = port;
        }
        for (int port = 0; port < NumPorts && chosen < 0; port++) {
            if (!pads[port] && controls.pad_names[port].empty()) chosen = port;
        }
        for (int port = 0; port < NumPorts && chosen < 0; port++) {
            if (!pads[port]) chosen = port;
        }
        if (chosen >= 0) {
            pads[chosen] = pad;
            std::fprintf(stderr, "[wg3d] input: \"%s\" -> port %d\n", name.c_str(), chosen + 1);
            return;
        }
        std::fprintf(stderr, "[wg3d] input: \"%s\" ignored (all 4 ports in use)\n", SDL_GameControllerName(pad));
        SDL_GameControllerClose(pad);
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
        assign_pad(pad);
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
    init_time = std::chrono::steady_clock::now();
    log_input = std::getenv("WG3D_INPUT_LOG") != nullptr;
    load_controls();
    if (const char* n = std::getenv("WG3D_FAKE_PADS")) {
        fake_pads = std::clamp(std::atoi(n), 0, NumPorts);
    }
    if (const char* s = std::getenv("WG3D_INPUT_SCRIPT")) {
        parse_script(s);
    }
    paks_override = std::getenv("WG3D_PAKS");
    refresh_paks();
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
    if (controls.keyboard_port >= 0) {
        next[controls.keyboard_port].connected = true;
        read_keyboard(next[controls.keyboard_port]);
    }
    // While the menu is open, and after it closes until every button and stick is released (so the
    // button that closed it doesn't also reach the game), the ports stay connected but idle.
    if (suppressed && !blocked) {
        bool held = false;
        for (const PortState& s : next) {
            held |= s.buttons != 0 || std::fabs(s.x) > 0.1f || std::fabs(s.y) > 0.1f;
        }
        suppressed = held;
    }
    if (blocked || suppressed) {
        for (PortState& s : next) {
            s.buttons = 0;
            s.x = s.y = 0.0f;
        }
    }

    std::lock_guard lock{ snapshot_mutex };
    snapshot = next;
}

void wg3d::input::on_vi() {
    if (ultramodern::is_game_started()) {
        if (game_vis++ == 0) {
            std::fprintf(stderr, "[wg3d] game started %.3f s after the window opened (script time 0)\n",
                         std::chrono::duration<double>(std::chrono::steady_clock::now() - init_time).count());
        }
    }
}

double wg3d::input::script_time() {
    return game_vis.load() / 60.0;
}

void wg3d::input::poll_input() {}

bool wg3d::input::get_input(int port, uint16_t* buttons, float* x, float* y) {
    if (port < 0 || port >= NumPorts) return false;
    std::lock_guard lock{ snapshot_mutex };
    PortState s = snapshot[port];
    // The script is applied here, on the game thread, so presses line up with VIs even when the main
    // thread's update() runs less often than VIs arrive (--speed).
    apply_script(port, s);
    if (log_input) {
        const PortState& a = last_logged[port];
        if (a.connected != s.connected || a.buttons != s.buttons || a.x != s.x || a.y != s.y) {
            std::fprintf(stderr, "[wg3d] input: t=%.3f port %d %s buttons=%04X stick=(%.2f, %.2f)\n", script_time(),
                         port + 1, s.connected ? "connected" : "absent", s.buttons, s.x, s.y);
            last_logged[port] = s;
        }
    }
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
    bool pak = false;
    if (port >= 0 && port < NumPorts) {
        std::lock_guard lock{ snapshot_mutex };
        connected = snapshot[port].connected;
        pak = pak_inserted[port];
    }
    if (!connected) {
        return { ultramodern::input::Device::None, ultramodern::input::Pak::None };
    }
    // Controller Pak: the runtime's osPfs* store each port's pak under
    // %APPDATA%/WG3DRecomp/saves/controllerpak_port<N>/.
    return { ultramodern::input::Device::Controller,
             pak ? ultramodern::input::Pak::ControllerPak : ultramodern::input::Pak::None };
}

// ---------------------------------------------------------------------------------------------
// Settings menu (main thread)
// ---------------------------------------------------------------------------------------------
int wg3d::input::num_targets() {
    return NumTargets;
}

const char* wg3d::input::target_name(int target) {
    return Targets[target].name;
}

int wg3d::input::max_bindings() {
    return MaxBindings;
}

nlohmann::json wg3d::input::controls_settings() {
    return controls_json;
}

nlohmann::json wg3d::input::default_controls_settings() {
    nlohmann::json j = controls_json;
    j["keyboard_port"] = 1;
    j["deadzone"] = 15;
    j["pads"] = nlohmann::json::array({ "", "", "", "" });
    for (int t = 0; t < NumTargets; t++) {
        j["keyboard"][Targets[t].name] = nlohmann::json::array();
        j["gamepad"][Targets[t].name] = nlohmann::json::array();
        for (int i = 0; i < MaxBindings; i++) {
            if (Defaults[t].keys[i]) j["keyboard"][Targets[t].name].push_back(Defaults[t].keys[i]);
            if (Defaults[t].pad[i]) j["gamepad"][Targets[t].name].push_back(Defaults[t].pad[i]);
        }
    }
    return j;
}

void wg3d::input::set_controls_settings(const nlohmann::json& settings) {
    controls_json = apply_controls(settings);
    write_controls(controls_json);
    // Port reservations may have changed: put the connected pads back on ports in their current order.
    std::array<SDL_GameController*, NumPorts> open = pads;
    pads = {};
    for (SDL_GameController* pad : open) {
        if (pad) assign_pad(pad);
    }
}

std::array<std::string, wg3d::input::NumPorts> wg3d::input::port_pad_names() {
    std::array<std::string, NumPorts> names{};
    for (int port = 0; port < NumPorts; port++) {
        if (pads[port] && SDL_GameControllerName(pads[port])) names[port] = SDL_GameControllerName(pads[port]);
    }
    return names;
}

void wg3d::input::refresh_paks() {
    std::array<bool, NumPorts> next{};
    if (paks_override) {
        // Port numbers with an inserted pak, e.g. "1", "1,2" or "none".
        for (const char* c = paks_override; *c; c++) {
            if (*c >= '1' && *c <= '0' + NumPorts) next[*c - '1'] = true;
        }
    } else {
        for (int port = 0; port < NumPorts; port++) next[port] = wg3d::config::pak_inserted(port);
    }
    static bool first = true;
    {
        std::lock_guard lock{ snapshot_mutex };
        if (next == pak_inserted && !first) return;
        pak_inserted = next;
        first = false;
    }
    std::fprintf(stderr, "[wg3d] input: controller paks in ports:%s%s%s%s\n", next[0] ? " 1" : "",
                 next[1] ? " 2" : "", next[2] ? " 3" : "", next[3] ? " 4" : "");
}

void wg3d::input::set_blocked(bool block) {
    if (blocked && !block) suppressed = true;
    blocked = block;
}
