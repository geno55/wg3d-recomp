// SPDX-License-Identifier: GPL-3.0-only
// The launcher and the settings menu, drawn with Dear ImGui (src/main/ui_render.cpp renders the frames).
//
// Launcher: ROM status, Start game, Choose ROM..., Settings, Quit. Shown on every normal start; test runs
// (--frames) skip it.
// Settings: General / Controls / Graphics / Sound tabs over the paused game, opened with Esc or a
// controller's Back (View) button. Every option is a row; changes apply at once and are saved to their
// file (wg3d::config::set_value, wg3d::input::set_controls_settings). General, Graphics and Sound rows
// come from the librecomp config schemas (src/main/config.cpp), so a new option there shows up here.
//
// Navigation is the menu's own, not ImGui's, so it behaves the same with a keyboard, a controller or a
// mouse: up/down select a row, left/right change its value, confirm activates, back closes, the
// shoulder buttons (Q/E) switch tabs. Button hints use the glyphs of the device used last (keyboard,
// Xbox, PlayStation or Nintendo layout).
//
// Everything here runs on the main thread (SDL events and update_gfx).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "SDL.h"

#include "imgui/imgui.h"
#include "imgui/imgui_impl_sdl2_custom.h"

#include "ultramodern/ultramodern.hpp"

#include "wg3d_config.h"
#include "wg3d_input.h"
#include "wg3d_ui.h"

namespace {
    using Clock = std::chrono::steady_clock;
    using wg3d::config::Section;

    // -----------------------------------------------------------------------------------------------
    // State
    // -----------------------------------------------------------------------------------------------
    enum class Screen { None, Launcher, Settings };
    enum Tab { TabGeneral, TabControls, TabGraphics, TabSound, NumTabs };
    const char* const TabNames[NumTabs] = { "General", "Controls", "Graphics", "Sound" };

    enum class Device { Keyboard, Xbox, PlayStation, Nintendo };
    enum class Glyph { Confirm, Back, Clear, TabPrev, TabNext, Menu };
    enum class Deferred { None, ChooseRom, StartGame, Quit };
    enum Dir { Up, Down, Left, Right, NumDirs };

    std::mutex frame_mtx;
    std::atomic<bool> shown{ false };
    SDL_Window* window = nullptr;
    wg3d::ui::LauncherCallbacks launcher;
    bool initialized = false;
    Screen screen = Screen::None;
    bool settings_from_launcher = false;
    bool focused = true;
    bool paused = false;
    Deferred deferred = Deferred::None;
    std::string launcher_message;
    int launcher_selected = -1;
    int tab = TabGeneral;
    std::array<int, NumTabs> selected{ -1, -1, -1, -1 };
    bool scroll_to_selected = false;
    bool quit_armed = false;  // "Quit game" asks for a second press

    // Controls tab
    int binding_device = 0;  // 0 keyboard, 1 gamepad
    int binding_slot = 0;
    struct Capture {
        bool active = false;
        int target = 0;
        int slot = 0;
        int device = 0;
        Clock::time_point start;
    } capture;
    constexpr auto CaptureTimeout = std::chrono::seconds(5);

    // Input, collected from events and consumed by the next frame.
    Device last_device = Device::Keyboard;
    struct Actions {
        int move[NumDirs] = {};
        bool confirm = false, back = false, clear = false, tab_prev = false, tab_next = false;
    } actions;
    std::array<bool, NumDirs> key_held{}, dpad_held{}, stick_held{};
    std::array<bool, NumDirs> was_held{};
    std::array<Clock::time_point, NumDirs> next_repeat{};
    std::array<int, NumDirs> repeats{};
    bool wait_release = false;  // after a capture: ignore directions until everything is let go
    bool mouse_moved = false;   // a real mouse motion event since the last frame (not the window moving)

    Clock::time_point last_build;
    float scale = 1.0f;
    ImFont* font_body = nullptr;
    ImFont* font_bold = nullptr;
    ImFont* font_title = nullptr;
    constexpr float BodyAtlasSize = 48.0f;
    constexpr float TitleAtlasSize = 112.0f;

    // Colours
    const ImU32 ColText = IM_COL32(232, 238, 245, 255);
    const ImU32 ColDim = IM_COL32(138, 150, 168, 255);
    const ImU32 ColAccent = IM_COL32(79, 163, 255, 255);
    const ImU32 ColSelected = IM_COL32(79, 163, 255, 52);
    const ImU32 ColPanel = IM_COL32(18, 24, 34, 246);
    const ImU32 ColGood = IM_COL32(110, 200, 120, 255);
    const ImU32 ColWarn = IM_COL32(240, 190, 80, 255);
    const ImU32 ColBad = IM_COL32(240, 100, 90, 255);

    // -----------------------------------------------------------------------------------------------
    // Devices and glyphs
    // -----------------------------------------------------------------------------------------------
    Device pad_device(SDL_JoystickID id) {
        SDL_GameController* pad = SDL_GameControllerFromInstanceID(id);
        switch (pad ? SDL_GameControllerGetType(pad) : SDL_CONTROLLER_TYPE_UNKNOWN) {
            case SDL_CONTROLLER_TYPE_PS3:
            case SDL_CONTROLLER_TYPE_PS4:
            case SDL_CONTROLLER_TYPE_PS5:
                return Device::PlayStation;
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_LEFT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT:
            case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_JOYCON_PAIR:
                return Device::Nintendo;
            default:
                return Device::Xbox;
        }
    }

    // The layout used to name gamepad inputs: the last controller used, Xbox if it was the keyboard.
    Device pad_layout() {
        return last_device == Device::Keyboard ? Device::Xbox : last_device;
    }

    // A controls.json gamepad input ("a", "leftshoulder", "lefty-", ...) as printed on the controller.
    std::string pad_input_label(const std::string& name) {
        const Device d = pad_layout();
        struct Label { const char* name; const char* xbox; const char* ps; const char* nintendo; };
        static const Label labels[] = {
            { "a", "A", "Cross", "B" }, { "b", "B", "Circle", "A" }, { "x", "X", "Square", "Y" },
            { "y", "Y", "Triangle", "X" }, { "back", "View", "Share", "-" }, { "guide", "Guide", "PS", "Home" },
            { "start", "Menu", "Options", "+" }, { "leftstick", "LS click", "L3", "LS click" },
            { "rightstick", "RS click", "R3", "RS click" }, { "leftshoulder", "LB", "L1", "L" },
            { "rightshoulder", "RB", "R1", "R" }, { "lefttrigger", "LT", "L2", "ZL" },
            { "righttrigger", "RT", "R2", "ZR" }, { "dpup", "D-pad up", "D-pad up", "D-pad up" },
            { "dpdown", "D-pad down", "D-pad down", "D-pad down" }, { "dpleft", "D-pad left", "D-pad left", "D-pad left" },
            { "dpright", "D-pad right", "D-pad right", "D-pad right" }, { "leftx-", "LS left", "LS left", "LS left" },
            { "leftx+", "LS right", "LS right", "LS right" }, { "lefty-", "LS up", "LS up", "LS up" },
            { "lefty+", "LS down", "LS down", "LS down" }, { "rightx-", "RS left", "RS left", "RS left" },
            { "rightx+", "RS right", "RS right", "RS right" }, { "righty-", "RS up", "RS up", "RS up" },
            { "righty+", "RS down", "RS down", "RS down" }, { "misc1", "Share", "Mic", "Capture" },
            { "touchpad", "Touchpad", "Touchpad", "Touchpad" },
        };
        for (const Label& l : labels) {
            if (name == l.name) return d == Device::PlayStation ? l.ps : d == Device::Nintendo ? l.nintendo : l.xbox;
        }
        return name;
    }

    // Draws a pill with text (keyboard keys, shoulder buttons); returns its width.
    float draw_pill(ImDrawList* dl, ImVec2 pos, float h, const char* text, ImU32 fill, ImU32 ink) {
        ImFont* f = font_bold;
        const float fs = h * 0.62f;
        const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0.0f, text);
        const float w = std::max(h, ts.x + h * 0.6f);
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), fill, h * 0.28f);
        dl->AddText(f, fs, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (h - ts.y) * 0.5f), ink, text);
        return w;
    }

    // A face button: a filled circle with a letter, or a PlayStation shape.
    float draw_face(ImDrawList* dl, ImVec2 pos, float h, int which /*0 south 1 east 2 west 3 north*/) {
        const ImVec2 c(pos.x + h * 0.5f, pos.y + h * 0.5f);
        const float r = h * 0.5f;
        const Device d = pad_layout();
        if (d == Device::PlayStation) {
            static const ImU32 colors[4] = { IM_COL32(124, 178, 232, 255), IM_COL32(255, 102, 102, 255),
                                             IM_COL32(231, 159, 196, 255), IM_COL32(64, 224, 176, 255) };
            dl->AddCircleFilled(c, r, IM_COL32(40, 44, 52, 255));
            const float s = r * 0.48f, t = std::max(1.5f, h * 0.09f);
            switch (which) {
                case 0:
                    dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), colors[0], t);
                    dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), colors[0], t);
                    break;
                case 1: dl->AddCircle(c, s, colors[1], 24, t); break;
                case 2: dl->AddRect(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), colors[2], 0.0f, 0, t); break;
                default:
                    dl->AddTriangle(ImVec2(c.x, c.y - s), ImVec2(c.x + s, c.y + s * 0.8f), ImVec2(c.x - s, c.y + s * 0.8f), colors[3], t);
                    break;
            }
            return h;
        }
        static const char* xbox[4] = { "A", "B", "X", "Y" };
        static const char* nintendo[4] = { "B", "A", "Y", "X" };
        static const ImU32 xbox_colors[4] = { IM_COL32(108, 194, 74, 255), IM_COL32(224, 71, 63, 255),
                                              IM_COL32(61, 139, 224, 255), IM_COL32(240, 196, 40, 255) };
        const char* letter = d == Device::Nintendo ? nintendo[which] : xbox[which];
        const ImU32 fill = d == Device::Nintendo ? IM_COL32(70, 74, 82, 255) : xbox_colors[which];
        dl->AddCircleFilled(c, r, fill);
        const float fs = h * 0.66f;
        const ImVec2 ts = font_bold->CalcTextSizeA(fs, FLT_MAX, 0.0f, letter);
        dl->AddText(font_bold, fs, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), IM_COL32(255, 255, 255, 255), letter);
        return h;
    }

    float draw_glyph(ImDrawList* dl, ImVec2 pos, float h, Glyph g) {
        const ImU32 key_fill = IM_COL32(58, 66, 80, 255);
        if (last_device == Device::Keyboard) {
            static const char* keys[] = { "Enter", "Esc", "Del", "Q", "E", "Esc" };
            return draw_pill(dl, pos, h, keys[(int)g], key_fill, ColText);
        }
        const Device d = last_device;
        switch (g) {
            case Glyph::Confirm: return draw_face(dl, pos, h, 0);
            case Glyph::Back: return draw_face(dl, pos, h, 1);
            case Glyph::Clear: return draw_face(dl, pos, h, 2);
            case Glyph::TabPrev:
                return draw_pill(dl, pos, h, d == Device::PlayStation ? "L1" : d == Device::Nintendo ? "L" : "LB", key_fill, ColText);
            case Glyph::TabNext:
                return draw_pill(dl, pos, h, d == Device::PlayStation ? "R1" : d == Device::Nintendo ? "R" : "RB", key_fill, ColText);
            case Glyph::Menu:
            default:
                return draw_pill(dl, pos, h, d == Device::PlayStation ? "Share" : d == Device::Nintendo ? "-" : "View", key_fill, ColText);
        }
    }

    // -----------------------------------------------------------------------------------------------
    // Rows
    // -----------------------------------------------------------------------------------------------
    struct Row {
        enum Kind { Header, Option, Action, Binding } kind = Option;
        std::string label;
        std::string value;
        std::string description;
        std::function<void(int)> change;  // Option: left/right
        std::function<void()> activate;   // confirm / click
        int target = -1;                  // Binding
        ImU32 color = 0;                  // Action: label colour (0 = normal)
    };

    Row header(const std::string& label) {
        Row r;
        r.kind = Row::Header;
        r.label = label;
        return r;
    }

    // Held-direction repeat count, for faster number changes.
    int held_repeats() {
        return std::max(repeats[Left], repeats[Right]);
    }

    void add_schema_rows(std::vector<Row>& rows, Section section) {
        using namespace recomp::config;
        const ConfigSchema& schema = wg3d::config::section_config(section).get_config_schema();
        for (const ConfigOption& opt : schema.options) {
            if (opt.hidden) continue;
            Row r;
            r.label = opt.name;
            r.description = opt.description;
            const std::string id = opt.id;
            const ConfigValueVariant value = wg3d::config::get_value(section, id);
            switch (opt.type) {
                case ConfigOptionType::Bool: {
                    const bool b = std::holds_alternative<bool>(value) && std::get<bool>(value);
                    r.value = b ? "On" : "Off";
                    r.change = [section, id, b](int) { wg3d::config::set_value(section, id, !b); };
                    break;
                }
                case ConfigOptionType::Enum: {
                    const auto& e = std::get<ConfigOptionEnum>(opt.variant);
                    const uint32_t v = std::holds_alternative<uint32_t>(value) ? std::get<uint32_t>(value) : e.default_value;
                    int index = 0;
                    for (int i = 0; i < (int)e.options.size(); i++) {
                        if (e.options[i].value == v) index = i;
                    }
                    r.value = e.options.empty() ? "" : e.options[index].name;
                    const int count = (int)e.options.size();
                    r.change = [section, id, e, index, count](int d) {
                        const int next = ((index + d) % count + count) % count;
                        wg3d::config::set_value(section, id, e.options[next].value);
                    };
                    break;
                }
                case ConfigOptionType::Number: {
                    const auto& n = std::get<ConfigOptionNumber>(opt.variant);
                    const double v = std::holds_alternative<double>(value) ? std::get<double>(value) : n.default_value;
                    const bool percent = id == "master_volume";
                    r.value = std::to_string((int)std::lround(v)) + (percent ? "%" : "");
                    const double step = percent ? 5.0 : 1.0;
                    r.change = [section, id, n, v, step](int d) {
                        const double fast = (step == 1.0 && held_repeats() > 8) ? 5.0 : 1.0;
                        const double next = std::clamp(v + d * step * fast, n.min, n.max);
                        if (next != v) wg3d::config::set_value(section, id, next);
                    };
                    break;
                }
                default:
                    continue;
            }
            rows.push_back(std::move(r));
        }
    }

    // Controls -----------------------------------------------------------------------------------
    std::string pad_input_name_from_event(const SDL_Event& e) {
        if (e.type == SDL_CONTROLLERBUTTONDOWN) {
            const char* n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)e.cbutton.button);
            return n ? n : "";
        }
        if (e.type == SDL_CONTROLLERAXISMOTION) {
            const int axis = e.caxis.axis;
            const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
            const float v = e.caxis.value / 32767.0f;
            if (trigger ? v < 0.5f : std::fabs(v) < 0.6f) return "";
            const char* n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)axis);
            if (!n) return "";
            return trigger ? std::string(n) : std::string(n) + (v < 0 ? "-" : "+");
        }
        return "";
    }

    const char* binding_section(int device) {
        return device == 0 ? "keyboard" : "gamepad";
    }

    std::vector<std::string> bindings_of(const nlohmann::json& controls, int device, int target) {
        std::vector<std::string> out;
        const auto section = controls.find(binding_section(device));
        if (section == controls.end() || !section->is_object()) return out;
        const auto list = section->find(wg3d::input::target_name(target));
        if (list == section->end() || !list->is_array()) return out;
        for (const auto& v : *list) {
            if (v.is_string()) out.push_back(v.get<std::string>());
        }
        return out;
    }

    // Puts `name` in a slot of a target's bindings (or clears the slot with ""), keeping each input once.
    void set_binding(int device, int target, int slot, const std::string& name) {
        nlohmann::json controls = wg3d::input::controls_settings();
        std::vector<std::string> list = bindings_of(controls, device, target);
        if (!name.empty()) {
            list.erase(std::remove(list.begin(), list.end(), name), list.end());
        }
        if (slot < (int)list.size()) {
            if (name.empty()) list.erase(list.begin() + slot);
            else list[slot] = name;
        } else if (!name.empty()) {
            list.push_back(name);
        }
        if ((int)list.size() > wg3d::input::max_bindings()) list.resize(wg3d::input::max_bindings());
        controls[binding_section(device)][wg3d::input::target_name(target)] = list;
        wg3d::input::set_controls_settings(controls);
    }

    std::string binding_label(int device, const std::string& name) {
        return device == 0 ? name : pad_input_label(name);
    }

    void add_controls_rows(std::vector<Row>& rows) {
        const nlohmann::json controls = wg3d::input::controls_settings();
        auto set_field = [](const char* key, const nlohmann::json& value) {
            nlohmann::json c = wg3d::input::controls_settings();
            c[key] = value;
            wg3d::input::set_controls_settings(c);
        };

        rows.push_back(header("Ports"));
        {
            const int port = controls.value("keyboard_port", 1);
            Row r;
            r.label = "Keyboard port";
            r.value = port == 0 ? "Off" : "Port " + std::to_string(port);
            r.description = "The controller port the keyboard plays on, merged with any controller on the same port.";
            r.change = [port, set_field](int d) { set_field("keyboard_port", ((port + d) % 5 + 5) % 5); };
            rows.push_back(r);
        }
        {
            const int dz = controls.value("deadzone", 15);
            Row r;
            r.label = "Stick deadzone";
            r.value = std::to_string(dz) + "%";
            r.description = "How far a controller's stick has to move before the game sees it.";
            r.change = [dz, set_field](int d) { set_field("deadzone", std::clamp(dz + d, 0, 50)); };
            rows.push_back(r);
        }
        const std::array<std::string, wg3d::input::NumPorts> on_port = wg3d::input::port_pad_names();
        for (int port = 0; port < wg3d::input::NumPorts; port++) {
            std::string want;
            if (controls["pads"].is_array() && port < (int)controls["pads"].size() && controls["pads"][port].is_string()) {
                want = controls["pads"][port].get<std::string>();
            }
            // Choices: any controller, each connected controller by name, and the saved name if it isn't here.
            std::vector<std::string> choices{ "" };
            for (const std::string& n : on_port) {
                if (!n.empty() && std::find(choices.begin(), choices.end(), n) == choices.end()) choices.push_back(n);
            }
            if (std::find(choices.begin(), choices.end(), want) == choices.end()) choices.push_back(want);
            const int index = (int)(std::find(choices.begin(), choices.end(), want) - choices.begin());
            Row r;
            r.label = "Port " + std::to_string(port + 1) + " controller";
            r.value = want.empty() ? "Any" : want;
            r.description = "Now on this port: " + (on_port[port].empty() ? std::string("nothing") : on_port[port]) +
                            ". Pick a controller to keep it on this port; Any takes controllers in the order they connect.";
            r.change = [port, choices, index](int d) {
                nlohmann::json c = wg3d::input::controls_settings();
                const int count = (int)choices.size();
                c["pads"][port] = choices[((index + d) % count + count) % count];
                wg3d::input::set_controls_settings(c);
            };
            rows.push_back(r);
        }

        rows.push_back(header("Bindings"));
        {
            Row r;
            r.label = "Bindings for";
            r.value = binding_device == 0 ? "Keyboard" : "Controllers";
            r.description = "Which bindings the rows below show. Controller bindings apply to every controller.";
            r.change = [](int) { binding_device = 1 - binding_device; };
            rows.push_back(r);
        }
        for (int t = 0; t < wg3d::input::num_targets(); t++) {
            Row r;
            r.kind = Row::Binding;
            r.label = wg3d::input::target_name(t);
            r.target = t;
            r.description = binding_device == 0
                ? "Choose a slot and press the key to use (Esc cancels)."
                : "Choose a slot and press the controller button or move the stick or trigger to use (Esc cancels).";
            rows.push_back(r);
        }
        {
            Row r;
            r.kind = Row::Action;
            r.label = "Reset controls to defaults";
            r.description = "Keyboard on port 1, 15% deadzone, any controller on any port, and the default bindings.";
            r.activate = [] { wg3d::input::set_controls_settings(wg3d::input::default_controls_settings()); };
            rows.push_back(r);
        }
    }

    std::vector<Row> build_rows(int which) {
        std::vector<Row> rows;
        switch (which) {
            case TabGeneral: {
                rows.push_back(header("Options"));
                add_schema_rows(rows, Section::General);
                if (ultramodern::is_game_started()) {
                    rows.push_back(header("Game"));
                    Row r;
                    r.kind = Row::Action;
                    r.label = quit_armed ? "Press again to quit" : "Quit game";
                    r.color = quit_armed ? ColBad : 0;
                    r.description = "Closes the game. Progress that hasn't been saved to the Controller Pak is lost.";
                    r.activate = [] {
                        if (quit_armed) deferred = Deferred::Quit;
                        quit_armed = !quit_armed;
                    };
                    rows.push_back(r);
                }
                break;
            }
            case TabControls: add_controls_rows(rows); break;
            case TabGraphics: add_schema_rows(rows, Section::Graphics); break;
            case TabSound: add_schema_rows(rows, Section::Sound); break;
        }
        return rows;
    }

    // -----------------------------------------------------------------------------------------------
    // Screens
    // -----------------------------------------------------------------------------------------------
    void close_settings() {
        capture.active = false;
        quit_armed = false;
        screen = settings_from_launcher ? Screen::Launcher : Screen::None;
    }

    void open_settings(bool from_launcher) {
        screen = Screen::Settings;
        settings_from_launcher = from_launcher;
        binding_device = last_device == Device::Keyboard ? 0 : 1;
        scroll_to_selected = true;
    }

    bool selectable_row(const std::vector<Row>& rows, int i) {
        return rows[i].kind != Row::Header;
    }

    void fix_selection(const std::vector<Row>& rows, int& sel) {
        if (sel >= 0 && sel < (int)rows.size() && selectable_row(rows, sel)) return;
        sel = -1;
        for (int i = 0; i < (int)rows.size() && sel < 0; i++) {
            if (selectable_row(rows, i)) sel = i;
        }
    }

    void move_selection(const std::vector<Row>& rows, int& sel, int d) {
        int i = sel;
        for (int n = 0; n < (int)rows.size(); n++) {
            i += d;
            if (i < 0 || i >= (int)rows.size()) return;  // stop at the ends
            if (selectable_row(rows, i)) {
                sel = i;
                scroll_to_selected = true;
                quit_armed = false;
                return;
            }
        }
    }

    // Applies the frame's actions to the selected row.
    void apply_actions(std::vector<Row>& rows) {
        int& sel = selected[tab];
        fix_selection(rows, sel);
        if (actions.tab_prev || actions.tab_next) {
            tab = (tab + (actions.tab_next ? 1 : NumTabs - 1)) % NumTabs;
            quit_armed = false;
            scroll_to_selected = true;
            return;
        }
        if (actions.back) {
            close_settings();
            return;
        }
        for (int n = 0; n < actions.move[Up]; n++) move_selection(rows, sel, -1);
        for (int n = 0; n < actions.move[Down]; n++) move_selection(rows, sel, +1);
        if (sel < 0) return;
        Row& row = rows[sel];
        const int lr = actions.move[Right] - actions.move[Left];
        if (row.kind == Row::Binding) {
            if (lr != 0) binding_slot = std::clamp(binding_slot + lr, 0, wg3d::input::max_bindings() - 1);
            if (actions.confirm) capture = { true, row.target, binding_slot, binding_device, Clock::now() };
            if (actions.clear) set_binding(binding_device, row.target, binding_slot, "");
            return;
        }
        if (row.kind == Row::Option && row.change) {
            for (int n = 0; n < actions.move[Right]; n++) row.change(+1);
            for (int n = 0; n < actions.move[Left]; n++) row.change(-1);
            if (actions.confirm) row.change(+1);
        } else if (row.kind == Row::Action && actions.confirm && row.activate) {
            row.activate();
        }
    }

    void draw_background(bool solid) {
        ImDrawList* bg = ImGui::GetBackgroundDrawList();
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        if (solid) {
            bg->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(16, 30, 52, 255), IM_COL32(16, 30, 52, 255),
                                        IM_COL32(4, 8, 14, 255), IM_COL32(4, 8, 14, 255));
            // A faint centre line and circle, like a rink.
            const float cx = size.x * 0.5f, cy = size.y * 0.5f;
            bg->AddLine(ImVec2(cx, 0), ImVec2(cx, size.y), IM_COL32(200, 40, 40, 40), 6.0f * scale);
            bg->AddCircle(ImVec2(cx, cy), size.y * 0.22f, IM_COL32(60, 120, 220, 40), 64, 5.0f * scale);
        } else {
            bg->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 150));
        }
    }

    // A row of button hints; each one is clickable. Returns the hint clicked, if any.
    struct Hint { Glyph glyph; const char* text; };
    int draw_hints(ImVec2 pos, float h, const std::vector<Hint>& hints, bool right_align, float right_edge) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float fs = h * 0.7f;
        const float gap = h * 0.35f, sep = h * 1.1f;
        // Measure first (drawing the glyphs off-screen), so the row can be right-aligned.
        float total = 0.0f;
        for (const Hint& hint : hints) {
            const ImVec2 off(-10000.0f, -10000.0f);
            float gw = draw_glyph(dl, off, h, hint.glyph);
            if (hint.glyph == Glyph::TabPrev) gw += h * 0.15f + draw_glyph(dl, off, h, Glyph::TabNext);
            total += gw + gap + font_body->CalcTextSizeA(fs, FLT_MAX, 0.0f, hint.text).x + sep;
        }
        float x = right_align ? right_edge - total + sep : pos.x;
        int clicked = -1;
        for (size_t i = 0; i < hints.size(); i++) {
            const ImVec2 p(x, pos.y);
            float gx = x;
            if (hints[i].glyph == Glyph::TabPrev) {
                gx += draw_glyph(dl, ImVec2(gx, p.y), h, Glyph::TabPrev) + h * 0.15f;
                gx += draw_glyph(dl, ImVec2(gx, p.y), h, Glyph::TabNext);
            } else {
                gx += draw_glyph(dl, ImVec2(gx, p.y), h, hints[i].glyph);
            }
            gx += gap;
            const ImVec2 ts = font_body->CalcTextSizeA(fs, FLT_MAX, 0.0f, hints[i].text);
            dl->AddText(font_body, fs, ImVec2(gx, p.y + (h - ts.y) * 0.5f), ColDim, hints[i].text);
            const float w = gx + ts.x - x;
            ImGui::SetCursorScreenPos(p);
            ImGui::PushID((int)i);
            if (ImGui::InvisibleButton("##hint", ImVec2(std::max(w, 1.0f), h))) clicked = (int)i;
            ImGui::PopID();
            x += w + sep;
        }
        return clicked;
    }

    void text_at(ImFont* font, float size, ImVec2 pos, ImU32 color, const std::string& text) {
        ImGui::GetWindowDrawList()->AddText(font, size, pos, color, text.c_str());
    }

    ImVec2 text_size(ImFont* font, float size, const std::string& text) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    }

    void build_launcher() {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        draw_background(true);
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(size);
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cx = size.x * 0.5f;

        const float title_size = 104.0f * scale;
        const std::string title = "W.G. 3D HOCKEY";
        ImVec2 ts = text_size(font_title, title_size, title);
        text_at(font_title, title_size, ImVec2(cx - ts.x * 0.5f, size.y * 0.16f), ColText, title);
        const float body = 34.0f * scale;
        const std::string sub = std::string("Recompiled  -  v") + WG3D_VERSION;
        ts = text_size(font_body, body, sub);
        text_at(font_body, body, ImVec2(cx - ts.x * 0.5f, size.y * 0.16f + title_size * 1.05f), ColAccent, sub);

        const bool rom_ok = launcher.rom_ready && launcher.rom_ready();
        const std::string status = rom_ok ? "ROM ready: Wayne Gretzky's 3D Hockey (USA)" : "No ROM yet: choose your copy of the game to play.";
        ts = text_size(font_body, body * 0.9f, status);
        const float status_y = size.y * 0.42f;
        text_at(font_body, body * 0.9f, ImVec2(cx - ts.x * 0.5f, status_y), rom_ok ? ColGood : ColWarn, status);
        if (!launcher_message.empty()) {
            float y = status_y + body * 1.3f;
            size_t start = 0;
            while (start <= launcher_message.size()) {
                size_t end = launcher_message.find('\n', start);
                std::string line = launcher_message.substr(start, end == std::string::npos ? std::string::npos : end - start);
                ts = text_size(font_body, body * 0.75f, line);
                text_at(font_body, body * 0.75f, ImVec2(cx - ts.x * 0.5f, y), ColBad, line);
                y += body * 0.95f;
                if (end == std::string::npos) break;
                start = end + 1;
            }
        }

        struct Item { const char* label; bool enabled; Deferred action; bool settings; };
        const Item items[] = {
            { "Start game", rom_ok, Deferred::StartGame, false },
            { rom_ok ? "Change ROM..." : "Choose ROM...", true, Deferred::ChooseRom, false },
            { "Settings", true, Deferred::None, true },
            { "Quit", true, Deferred::Quit, false },
        };
        constexpr int count = 4;
        if (launcher_selected < 0 || !items[launcher_selected].enabled) launcher_selected = rom_ok ? 0 : 1;
        auto step = [&](int d) {
            for (int i = launcher_selected + d; i >= 0 && i < count; i += d) {
                if (items[i].enabled) { launcher_selected = i; return; }
            }
        };
        for (int n = 0; n < actions.move[Up]; n++) step(-1);
        for (int n = 0; n < actions.move[Down]; n++) step(+1);
        int activate = actions.confirm ? launcher_selected : -1;

        const float item_w = 560.0f * scale, item_h = 68.0f * scale;
        float y = size.y * 0.55f;
        for (int i = 0; i < count; i++) {
            const ImVec2 p0(cx - item_w * 0.5f, y), p1(cx + item_w * 0.5f, y + item_h);
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(i);
            const bool clicked = ImGui::InvisibleButton("##item", ImVec2(item_w, item_h));
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered && items[i].enabled && mouse_moved) {
                launcher_selected = i;
            }
            if (clicked && items[i].enabled) activate = i;
            const bool sel = i == launcher_selected;
            if (sel) {
                dl->AddRectFilled(p0, p1, ColSelected, 10.0f * scale);
                dl->AddRect(p0, p1, ColAccent, 10.0f * scale, 0, 2.0f * scale);
            }
            const float fs = 38.0f * scale;
            ts = text_size(font_bold, fs, items[i].label);
            text_at(font_bold, fs, ImVec2(cx - ts.x * 0.5f, y + (item_h - ts.y) * 0.5f),
                    items[i].enabled ? (sel ? ColText : ColDim) : IM_COL32(80, 88, 100, 255), items[i].label);
            y += item_h + 10.0f * scale;
        }
        if (activate >= 0) {
            if (items[activate].settings) {
                open_settings(true);
            } else {
                deferred = items[activate].action;
            }
        }

        const float hint_h = 36.0f * scale;
        draw_hints(ImVec2(0, size.y - hint_h - 28.0f * scale), hint_h, { { Glyph::Confirm, "Select" } }, true, size.x - 36.0f * scale);
        ImGui::End();
    }

    void build_settings() {
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        draw_background(settings_from_launcher);
        std::vector<Row> rows = build_rows(tab);
        if (!capture.active) apply_actions(rows);
        if (screen != Screen::Settings) return;  // closed by an action
        rows = build_rows(tab);
        int& sel = selected[tab];
        fix_selection(rows, sel);

        const float w = std::min(1320.0f * scale, size.x - 32.0f * scale);
        const float h = std::min(940.0f * scale, size.y - 32.0f * scale);
        const ImVec2 origin((size.x - w) * 0.5f, (size.y - h) * 0.5f);
        ImGui::SetNextWindowPos(origin);
        ImGui::SetNextWindowSize(ImVec2(w, h));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(ColPanel));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("##settings", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float pad = 36.0f * scale;

        // Tabs
        int clicked_tab = -1;
        const float tab_y = origin.y + 28.0f * scale;
        const float tab_fs = 36.0f * scale;
        const float glyph_h = 38.0f * scale;
        float x = origin.x + pad;
        x += draw_glyph(dl, ImVec2(x, tab_y + 4.0f * scale), glyph_h, Glyph::TabPrev) + 24.0f * scale;
        for (int t = 0; t < NumTabs; t++) {
            const ImVec2 ts = text_size(font_bold, tab_fs, TabNames[t]);
            const ImVec2 p0(x - 14.0f * scale, tab_y), p1(x + ts.x + 14.0f * scale, tab_y + 48.0f * scale);
            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(t);
            if (ImGui::InvisibleButton("##tab", ImVec2(p1.x - p0.x, p1.y - p0.y)) && t != tab) {
                clicked_tab = t;  // switched after this frame: the rows below were built for `tab`
            }
            ImGui::PopID();
            const bool active = t == tab;
            if (active) dl->AddRectFilled(ImVec2(p0.x, p1.y - 4.0f * scale), p1, ColAccent, 2.0f * scale);
            text_at(font_bold, tab_fs, ImVec2(x, tab_y + (48.0f * scale - ts.y) * 0.5f - 2.0f * scale), active ? ColText : ColDim, TabNames[t]);
            x += ts.x + 52.0f * scale;
        }
        x += -28.0f * scale;
        draw_glyph(dl, ImVec2(x, tab_y + 4.0f * scale), glyph_h, Glyph::TabNext);
        const std::string where = ultramodern::is_game_started() ? "Game paused" : "Settings";
        const ImVec2 ws = text_size(font_body, 30.0f * scale, where);
        text_at(font_body, 30.0f * scale, ImVec2(origin.x + w - pad - ws.x, tab_y + 8.0f * scale), ColDim, where);
        const float rows_top = tab_y + 72.0f * scale;
        dl->AddLine(ImVec2(origin.x + pad, rows_top - 8.0f * scale), ImVec2(origin.x + w - pad, rows_top - 8.0f * scale), IM_COL32(255, 255, 255, 30), 1.5f * scale);

        // Rows
        const float footer_h = 150.0f * scale;
        const float rows_h = origin.y + h - footer_h - rows_top;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + pad, rows_top));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 0));
        ImGui::BeginChild("##rows", ImVec2(w - 2 * pad, rows_h), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
        ImDrawList* rl = ImGui::GetWindowDrawList();
        const float row_w = ImGui::GetContentRegionAvail().x;
        const float row_h = 58.0f * scale;
        const float fs = 32.0f * scale;
        const float cell_w = 300.0f * scale;
        const nlohmann::json controls = wg3d::input::controls_settings();
        const ImGuiIO& io = ImGui::GetIO();
        for (int i = 0; i < (int)rows.size(); i++) {
            const Row& row = rows[i];
            const ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (row.kind == Row::Header) {
                const float hh = 56.0f * scale;
                ImGui::Dummy(ImVec2(row_w, hh));
                text_at(font_bold, 26.0f * scale, ImVec2(p0.x + 12.0f * scale, p0.y + hh - 34.0f * scale), ColAccent, row.label);
                continue;
            }
            ImGui::PushID(i);
            ImGui::InvisibleButton("##row", ImVec2(row_w, row_h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
            const bool hovered = ImGui::IsItemHovered();
            const bool left_click = ImGui::IsItemClicked(ImGuiMouseButton_Left);
            const bool right_click = ImGui::IsItemClicked(ImGuiMouseButton_Right);
            ImGui::PopID();
            if (hovered && mouse_moved && !capture.active) {
                if (sel != i) quit_armed = false;
                sel = i;
            }
            const bool is_sel = i == sel;
            const ImVec2 p1(p0.x + row_w, p0.y + row_h);
            if (is_sel) {
                rl->AddRectFilled(p0, p1, ColSelected, 8.0f * scale);
                rl->AddRectFilled(p0, ImVec2(p0.x + 5.0f * scale, p1.y), ColAccent, 2.0f * scale);
            }
            const float ty = p0.y + (row_h - fs) * 0.5f - 2.0f * scale;
            text_at(font_body, fs, ImVec2(p0.x + 22.0f * scale, ty), row.color ? row.color : (is_sel ? ColText : IM_COL32(200, 208, 220, 255)), row.label);

            if (row.kind == Row::Option) {
                const ImVec2 vs = text_size(font_bold, fs, row.value);
                const float arrows = 30.0f * scale;
                const float vx = p1.x - 24.0f * scale - arrows - vs.x;
                text_at(font_bold, fs, ImVec2(vx, ty), is_sel ? ColText : ColDim, row.value);
                if (is_sel) {
                    text_at(font_bold, fs, ImVec2(vx - arrows - 6.0f * scale, ty), ColAccent, "<");
                    text_at(font_bold, fs, ImVec2(p1.x - 24.0f * scale - arrows * 0.6f, ty), ColAccent, ">");
                }
                if ((left_click || right_click) && row.change && !capture.active) {
                    sel = i;
                    const bool left_half = io.MousePos.x < vx - 4.0f * scale;
                    row.change(right_click || (left_click && left_half && io.MousePos.x > vx - arrows * 2.0f) ? -1 : +1);
                }
            } else if (row.kind == Row::Action) {
                if (left_click && row.activate && !capture.active) {
                    sel = i;
                    row.activate();
                }
            } else if (row.kind == Row::Binding) {
                const std::vector<std::string> list = bindings_of(controls, binding_device, row.target);
                for (int slot = 0; slot < wg3d::input::max_bindings(); slot++) {
                    const float cx0 = p1.x - (wg3d::input::max_bindings() - slot) * (cell_w + 12.0f * scale);
                    const ImVec2 c0(cx0, p0.y + 7.0f * scale), c1(cx0 + cell_w, p1.y - 7.0f * scale);
                    const bool capturing = capture.active && capture.target == row.target && capture.slot == slot &&
                                           capture.device == binding_device;
                    const bool cell_sel = is_sel && slot == binding_slot;
                    rl->AddRectFilled(c0, c1, cell_sel ? IM_COL32(79, 163, 255, 90) : IM_COL32(255, 255, 255, 14), 6.0f * scale);
                    if (cell_sel) rl->AddRect(c0, c1, ColAccent, 6.0f * scale, 0, 2.0f * scale);
                    std::string text = slot < (int)list.size() ? binding_label(binding_device, list[slot]) : "-";
                    ImU32 color = slot < (int)list.size() ? ColText : ColDim;
                    if (capturing) {
                        text = binding_device == 0 ? "Press a key..." : "Press a button...";
                        color = ColWarn;
                    }
                    const float cfs = fs * 0.88f;
                    const ImVec2 cs = text_size(font_body, cfs, text);
                    rl->PushClipRect(c0, c1, true);
                    text_at(font_body, cfs, ImVec2(c0.x + std::max(8.0f * scale, (cell_w - cs.x) * 0.5f), c0.y + (c1.y - c0.y - cs.y) * 0.5f), color, text);
                    rl->PopClipRect();
                    const bool in_cell = io.MousePos.x >= c0.x && io.MousePos.x < c1.x;
                    if (hovered && in_cell && mouse_moved && !capture.active) binding_slot = slot;
                    if (in_cell && !capture.active) {
                        if (left_click) {
                            sel = i;
                            binding_slot = slot;
                            capture = { true, row.target, slot, binding_device, Clock::now() };
                        } else if (right_click) {
                            set_binding(binding_device, row.target, slot, "");
                        }
                    }
                }
            }
            if (is_sel && scroll_to_selected) {
                ImGui::SetScrollHereY(0.5f);
                scroll_to_selected = false;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);

        // Description and hints
        const float desc_y = origin.y + h - footer_h + 12.0f * scale;
        dl->AddLine(ImVec2(origin.x + pad, desc_y - 6.0f * scale), ImVec2(origin.x + w - pad, desc_y - 6.0f * scale), IM_COL32(255, 255, 255, 30), 1.5f * scale);
        if (sel >= 0 && sel < (int)rows.size() && !rows[sel].description.empty()) {
            const float dfs = 27.0f * scale;
            ImGui::GetWindowDrawList()->AddText(font_body, dfs, ImVec2(origin.x + pad, desc_y + 4.0f * scale), ColDim,
                                                rows[sel].description.c_str(), nullptr, w - 2 * pad);
        }
        const float hint_h = 36.0f * scale;
        std::vector<Hint> hints;
        const bool binding_row = sel >= 0 && sel < (int)rows.size() && rows[sel].kind == Row::Binding;
        if (capture.active) {
            hints.push_back({ Glyph::Back, "Cancel (Esc)" });
        } else {
            hints.push_back({ Glyph::Confirm, binding_row ? "Change" : "Select" });
            if (binding_row) hints.push_back({ Glyph::Clear, "Clear" });
            hints.push_back({ Glyph::TabPrev, "Tabs" });
            hints.push_back({ Glyph::Back, settings_from_launcher ? "Back" : "Resume" });
        }
        const int clicked = draw_hints(ImVec2(origin.x + pad, origin.y + h - hint_h - 24.0f * scale), hint_h, hints, true, origin.x + w - pad);
        if (clicked >= 0) {
            if (capture.active) {
                capture.active = false;
            } else if (hints[clicked].glyph == Glyph::Back) {
                close_settings();
            } else if (hints[clicked].glyph == Glyph::TabPrev) {
                tab = (tab + 1) % NumTabs;
            } else if (hints[clicked].glyph == Glyph::Clear && binding_row) {
                set_binding(binding_device, rows[sel].target, binding_slot, "");
            } else if (hints[clicked].glyph == Glyph::Confirm && sel >= 0) {
                if (binding_row) capture = { true, rows[sel].target, binding_slot, binding_device, Clock::now() };
                else if (rows[sel].kind == Row::Option && rows[sel].change) rows[sel].change(+1);
                else if (rows[sel].kind == Row::Action && rows[sel].activate) rows[sel].activate();
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(2);
        if (clicked_tab >= 0) {
            tab = clicked_tab;
            quit_armed = false;
            scroll_to_selected = true;
        }
        ImGui::PopStyleColor();
    }

    // -----------------------------------------------------------------------------------------------
    // Setup
    // -----------------------------------------------------------------------------------------------
    void load_fonts() {
        ImGuiIO& io = ImGui::GetIO();
        std::filesystem::path fonts = "C:\\Windows\\Fonts";
        if (const char* windir = std::getenv("WINDIR")) fonts = std::filesystem::path(windir) / "Fonts";
        auto add = [&](const char* file, float size) -> ImFont* {
            const std::filesystem::path path = fonts / file;
            if (!std::filesystem::exists(path)) return nullptr;
            ImFontConfig cfg;
            cfg.OversampleH = 2;
            cfg.OversampleV = 2;
            return io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &cfg);
        };
        font_body = add("segoeui.ttf", BodyAtlasSize);
        font_bold = add("segoeuib.ttf", BodyAtlasSize);
        font_title = add("seguibl.ttf", TitleAtlasSize);
        if (!font_title) font_title = add("segoeuib.ttf", TitleAtlasSize);
        if (!font_body) {
            std::fprintf(stderr, "[wg3d] ui: Segoe UI not found, using ImGui's built-in font\n");
            ImFontConfig cfg;
            cfg.SizePixels = BodyAtlasSize;
            font_body = io.Fonts->AddFontDefault(&cfg);
        }
        if (!font_bold) font_bold = font_body;
        if (!font_title) font_title = font_bold;
        io.Fonts->Build();
    }

    void update_pause() {
        const bool want = ultramodern::is_game_started() &&
                          (screen == Screen::Settings || (!focused && wg3d::config::pause_when_unfocused()));
        if (want != paused) {
            paused = want;
            ultramodern::set_game_paused(paused);
            std::fprintf(stderr, "[wg3d] game %s\n", paused ? "paused" : "resumed");
        }
        wg3d::input::set_blocked(screen != Screen::None);
    }

    void press(Dir d, bool down, std::array<bool, NumDirs>& held) {
        held[d] = down;
    }
}

std::mutex& wg3d::ui::frame_mutex() {
    return frame_mtx;
}

bool wg3d::ui::visible() {
    return shown.load();
}

void wg3d::ui::init(SDL_Window* w, const LauncherCallbacks* l) {
    window = w;
    std::lock_guard lock{ frame_mtx };
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplSDL2_InitForOther(window);
    load_fonts();
    initialized = true;
    if (l) {
        launcher = *l;
        launcher_message = l->message;
        screen = Screen::Launcher;
    }
    shown = screen != Screen::None;
}

void wg3d::ui::handle_event(const SDL_Event& e) {
    if (!initialized) return;
    if (e.type == SDL_WINDOWEVENT) {
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focused = true;
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focused = false;
    }

    // Binding capture takes the next key or controller input.
    if (capture.active) {
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
                capture.active = false;
            } else if (capture.device == 0) {
                set_binding(0, capture.target, capture.slot, SDL_GetScancodeName(e.key.keysym.scancode));
                capture.active = false;
            }
            wait_release = true;
            last_device = Device::Keyboard;
        } else if (e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERAXISMOTION) {
            const std::string name = pad_input_name_from_event(e);
            if (!name.empty()) {
                last_device = pad_device(e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.which : e.caxis.which);
                if (capture.device == 1) {
                    set_binding(1, capture.target, capture.slot, name);
                    capture.active = false;
                } else if (name == "b") {
                    capture.active = false;  // keyboard capture: the controller's Back button cancels
                }
                wait_release = true;
            }
        } else if (e.type == SDL_MOUSEBUTTONDOWN) {
            capture.active = false;
        }
        return;
    }

    // Esc / Back (View): open the menu in game, or close it.
    const bool menu_key = (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) ||
                          (e.type == SDL_CONTROLLERBUTTONDOWN &&
                           (e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK || e.cbutton.button == SDL_CONTROLLER_BUTTON_GUIDE));
    if (menu_key) {
        last_device = e.type == SDL_KEYDOWN ? Device::Keyboard : pad_device(e.cbutton.which);
        if (screen == Screen::Settings) {
            actions.back = true;
        } else if (screen == Screen::None && ultramodern::is_game_started()) {
            open_settings(false);
        }
        return;
    }
    if (screen == Screen::None) return;

    switch (e.type) {
        case SDL_MOUSEMOTION:
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
        case SDL_MOUSEWHEEL: {
            std::lock_guard lock{ frame_mtx };
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_MOUSEMOTION) {
                // Windows also reports motion when the window moves or resizes under a still cursor.
                static int last_x = INT32_MIN, last_y = INT32_MIN;
                int x, y;
                SDL_GetGlobalMouseState(&x, &y);
                mouse_moved |= x != last_x || y != last_y;
                last_x = x;
                last_y = y;
            } else {
                last_device = Device::Keyboard;
            }
            break;
        }
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            const bool down = e.type == SDL_KEYDOWN;
            last_device = Device::Keyboard;
            switch (e.key.keysym.scancode) {
                case SDL_SCANCODE_UP: press(Up, down, key_held); break;
                case SDL_SCANCODE_DOWN: press(Down, down, key_held); break;
                case SDL_SCANCODE_LEFT: press(Left, down, key_held); break;
                case SDL_SCANCODE_RIGHT: press(Right, down, key_held); break;
                default:
                    if (!down || e.key.repeat) break;
                    switch (e.key.keysym.scancode) {
                        case SDL_SCANCODE_RETURN:
                        case SDL_SCANCODE_KP_ENTER:
                        case SDL_SCANCODE_SPACE:
                            if (!(e.key.keysym.mod & KMOD_ALT)) actions.confirm = true;
                            break;
                        case SDL_SCANCODE_BACKSPACE: actions.back = true; break;
                        case SDL_SCANCODE_DELETE: actions.clear = true; break;
                        case SDL_SCANCODE_Q:
                        case SDL_SCANCODE_PAGEUP: actions.tab_prev = true; break;
                        case SDL_SCANCODE_E:
                        case SDL_SCANCODE_PAGEDOWN: actions.tab_next = true; break;
                        default: break;
                    }
            }
            break;
        }
        case SDL_CONTROLLERBUTTONDOWN:
        case SDL_CONTROLLERBUTTONUP: {
            const bool down = e.type == SDL_CONTROLLERBUTTONDOWN;
            if (down) last_device = pad_device(e.cbutton.which);
            switch (e.cbutton.button) {
                case SDL_CONTROLLER_BUTTON_DPAD_UP: press(Up, down, dpad_held); break;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN: press(Down, down, dpad_held); break;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT: press(Left, down, dpad_held); break;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: press(Right, down, dpad_held); break;
                case SDL_CONTROLLER_BUTTON_A:
                case SDL_CONTROLLER_BUTTON_START: if (down) actions.confirm = true; break;
                case SDL_CONTROLLER_BUTTON_B: if (down) actions.back = true; break;
                case SDL_CONTROLLER_BUTTON_X: if (down) actions.clear = true; break;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: if (down) actions.tab_prev = true; break;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: if (down) actions.tab_next = true; break;
                default: break;
            }
            break;
        }
        case SDL_CONTROLLERAXISMOTION: {
            // Left stick as a d-pad, with hysteresis.
            const float v = e.caxis.value / 32767.0f;
            auto axis = [&](Dir neg, Dir pos) {
                if (std::fabs(v) > 0.6f) last_device = pad_device(e.caxis.which);
                stick_held[neg] = stick_held[neg] ? v < -0.35f : v < -0.6f;
                stick_held[pos] = stick_held[pos] ? v > 0.35f : v > 0.6f;
            };
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTX) axis(Left, Right);
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) axis(Up, Down);
            break;
        }
        default:
            break;
    }
}

void wg3d::ui::update() {
    if (!initialized) return;
    const Clock::time_point now = Clock::now();

    // Actions picked in the last frame that must not run while frame_mutex is held (a file dialog, the
    // game start, quitting).
    if (deferred != Deferred::None) {
        const Deferred d = deferred;
        deferred = Deferred::None;
        if (d == Deferred::ChooseRom && launcher.choose_rom) {
            launcher_message = launcher.choose_rom();
        } else if (d == Deferred::StartGame && launcher.start_game) {
            screen = Screen::None;
            launcher.start_game();
        } else if (d == Deferred::Quit) {
            std::fprintf(stderr, "[wg3d] quit from the menu\n");
            ultramodern::quit();
        }
    }
    if (capture.active && now - capture.start > CaptureTimeout) capture.active = false;
    update_pause();
    shown = screen != Screen::None;
    if (screen == Screen::None) {
        actions = {};
        return;
    }

    // Held directions: one step on press, then repeats.
    bool any_held = false;
    for (int d = 0; d < NumDirs; d++) {
        const bool held = key_held[d] || dpad_held[d] || stick_held[d];
        any_held |= held;
        if (wait_release) continue;
        if (held && !was_held[d]) {
            actions.move[d]++;
            next_repeat[d] = now + std::chrono::milliseconds(400);
            repeats[d] = 0;
        } else if (held && now >= next_repeat[d]) {
            actions.move[d]++;
            repeats[d]++;
            next_repeat[d] = now + std::chrono::milliseconds(70);
        }
        was_held[d] = held;
    }
    if (wait_release && !any_held) {
        wait_release = false;
        was_held = {};
    }

    if (now - last_build < std::chrono::milliseconds(8) || !backend_ready()) return;
    int w = 0, h = 0;
    SDL_GetWindowSize(window, &w, &h);
    if (w <= 0 || h <= 0 || (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED)) return;
    last_build = now;

    std::lock_guard lock{ frame_mtx };
    ImGui_ImplSDL2_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
    const float s = std::max(0.45f, std::min(io.DisplaySize.y / 1080.0f, io.DisplaySize.x / 1440.0f));
    if (s != scale) {
        scale = s;
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        ImGui::StyleColorsDark(&style);
        style.ScrollbarSize = 14.0f;
        style.ScrollbarRounding = 7.0f;
        style.ScaleAllSizes(scale);
    }
    io.FontGlobalScale = scale * 32.0f / BodyAtlasSize;
    ImGui::NewFrame();
    if (screen == Screen::Launcher) {
        build_launcher();
    } else {
        build_settings();
    }
    ImGui::Render();
    actions = {};
    mouse_moved = false;
    shown = screen != Screen::None;
}
