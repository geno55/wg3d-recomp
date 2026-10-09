// W.G. 3D Hockey: Recompiled -- entry point and runtime glue (chunk 3.2).
// Structure follows Quest64-Recomp's src/main/main.cpp (SDL window/audio, RT64 renderer, RSP ucode
// lookup), without its RmlUi launcher: the ROM comes from the command line (or the copy the runtime
// stored on a previous run) and the game starts immediately.
//
// Usage: wg3d.exe [--frames N] [--speed N] [--data-dir DIR] [--console] [path/to/rom.{z64,v64,n64}]
#include <array>
#include <atomic>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <xmmintrin.h>

#define SDL_MAIN_HANDLED
#include "SDL.h"
#include "SDL_syswm.h"
#include "nfd.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#endif

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/game.hpp"
#include "librecomp/rsp.hpp"

#include "wg3d_capture.h"
#include "wg3d_config.h"
#include "wg3d_crash.h"
#include "wg3d_input.h"
#include "wg3d_log.h"
#include "wg3d_render.h"
#include "wg3d_stats.h"
#include "wg3d_trace.h"

namespace wg3d {
    void register_overlays();
}

static const std::string version_string = WG3D_VERSION;  // CMakeLists.txt project(VERSION)
static std::u8string game_id = u8"wg3d.us";
// Set from the command line in main() (chunk 3.7).
static uint64_t smoke_frames = 0;
static std::filesystem::path data_dir_override;
// --speed N: run the game N times faster than real time (test runs). Audio is muted when N > 1.
static uint32_t speed = 1;

[[noreturn]] static void exit_error(const std::string& msg) {
    fprintf(stderr, "%s\n", msg.c_str());
    // No dialog in --frames runs: those are automated and would hang on it (chunk 5.2).
    if (smoke_frames == 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "W.G. 3D Hockey: Recompiled", msg.c_str(), nullptr);
    }
    std::exit(EXIT_FAILURE);
}

// ---------------------------------------------------------------------------------------------
// Window / gfx callbacks
// ---------------------------------------------------------------------------------------------
static SDL_Window* window = nullptr;

static ultramodern::gfx_callbacks_t::gfx_data_t create_gfx() {
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) {
        exit_error(std::string("Failed to initialize SDL2: ") + SDL_GetError());
    }
    fprintf(stdout, "SDL video driver: %s\n", SDL_GetCurrentVideoDriver());
    return {};
}

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t) {
    uint32_t flags = SDL_WINDOW_RESIZABLE;
#if defined(RT64_SDL_WINDOW_VULKAN)
    flags |= SDL_WINDOW_VULKAN;
#endif
    // Last session's windowed placement (window.json); centred on first run or if it is off-screen.
    wg3d::config::WindowState placement = wg3d::config::window_state();
    int x = placement.has_position ? placement.x : SDL_WINDOWPOS_CENTERED;
    int y = placement.has_position ? placement.y : SDL_WINDOWPOS_CENTERED;
    window = SDL_CreateWindow("W.G. 3D Hockey: Recompiled", x, y, placement.width, placement.height, flags);
    if (window == nullptr) {
        exit_error(std::string("Failed to create window: ") + SDL_GetError());
    }
    if (placement.maximized && ultramodern::renderer::get_graphics_config().wm_option != ultramodern::renderer::WindowMode::Fullscreen) {
        SDL_MaximizeWindow(window);
    }
    SDL_SysWMinfo wm_info;
    SDL_VERSION(&wm_info.version);
    SDL_GetWindowWMInfo(window, &wm_info);
    // Before the game thread starts, so pads present at launch are on their ports for osContInit.
    wg3d::input::init();
    wg3d::capture::init();
    wg3d::capture::set_render_only_for_captures(speed > 1);
#if defined(_WIN32)
    return ultramodern::renderer::WindowHandle{ wm_info.info.win.window, GetCurrentThreadId() };
#else
    return ultramodern::renderer::WindowHandle{ window };
#endif
}

std::atomic<uint64_t> wg3d::stats::display_lists{ 0 };
std::atomic<uint64_t> wg3d::stats::screen_updates{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_tasks{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_buffers{ 0 };
std::atomic<uint64_t> wg3d::stats::origin_changes{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_peak{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_samples{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_out_peak_milli{ 0 };
std::atomic<uint64_t> wg3d::stats::audio_out_bytes{ 0 };

static void print_stats();

static void update_gfx(void*) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT) {
            ultramodern::quit();
        } else if (event.type == SDL_WINDOWEVENT) {
            wg3d::config::track_window(window);
        } else if (event.type == SDL_KEYDOWN && !event.key.repeat) {
            // Alt+Enter / F11: windowed <-> borderless fullscreen (chunk 5.2). input.cpp ignores Enter
            // while Alt is held, so this doesn't also press Start.
            SDL_Keycode key = event.key.keysym.sym;
            if (key == SDLK_F11 || (key == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT))) {
                wg3d::config::toggle_fullscreen();
            }
        }
        wg3d::input::handle_event(event);
    }
    wg3d::input::update();
    wg3d::capture::update(window);
    print_stats();
    // Counted in game time (VIs since the game started), so --frames means the same at any --speed.
    if (smoke_frames != 0 && wg3d::input::script_time() * 60.0 >= (double)smoke_frames) {
        static bool quitting = false;
        if (!quitting) {
            quitting = true;
            fprintf(stderr, "[wg3d] smoke: %" PRIu64 " frames reached (dls=%" PRIu64 " audtasks=%" PRIu64 " aibufs=%" PRIu64 "), quitting\n",
                    wg3d::stats::screen_updates.load(), wg3d::stats::display_lists.load(), wg3d::stats::audio_tasks.load(),
                    wg3d::stats::audio_buffers.load());
            ultramodern::quit();
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Audio callbacks (Quest64-Recomp's SDL queue implementation, without the volume setting).
// A "frame" is one sample per channel.
// ---------------------------------------------------------------------------------------------
static SDL_AudioCVT audio_convert;
static SDL_AudioDeviceID audio_device = 0;
static uint32_t sample_rate = 48000;
static uint32_t output_sample_rate = 48000;
constexpr uint32_t input_channels = 2;
static uint32_t output_channels = 2;
constexpr uint32_t duplicated_input_frames = 4;  // frames reused across chunks to keep resampling smooth
static uint32_t discarded_output_frames;
constexpr uint32_t bytes_per_frame = input_channels * sizeof(float);

// WG3D_AUDIO_DUMP=<file.wav>: writes the game's raw samples (after the channel swap, at the game's
// rate) for the first 60 seconds, for checking pitch/content against ares (chunk 3.4).
static FILE* audio_dump = nullptr;
static uint32_t audio_dump_frames = 0;
static uint32_t audio_dump_rate = 0;

static void write_wav_header(FILE* f, uint32_t rate, uint32_t frames) {
    auto u32 = [f](uint32_t v) { fwrite(&v, 4, 1, f); };
    auto u16 = [f](uint16_t v) { fwrite(&v, 2, 1, f); };
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); u32(36 + frames * 4); fwrite("WAVEfmt ", 1, 8, f);
    u32(16); u16(1); u16(2); u32(rate); u32(rate * 4); u16(4); u16(16);
    fwrite("data", 1, 4, f); u32(frames * 4);
    fseek(f, 0, SEEK_END);
}

static void dump_audio(const int16_t* audio_data, size_t sample_count) {
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (const char* path = std::getenv("WG3D_AUDIO_DUMP")) {
            audio_dump = fopen(path, "wb");
            if (audio_dump) write_wav_header(audio_dump, sample_rate, 0);
        }
    }
    if (!audio_dump) return;
    if (audio_dump_rate == 0) audio_dump_rate = sample_rate;
    for (size_t i = 0; i + 1 < sample_count; i += 2) {
        int16_t lr[2] = { audio_data[i + 1], audio_data[i + 0] };
        fwrite(lr, 2, 2, audio_dump);
    }
    audio_dump_frames += (uint32_t)(sample_count / 2);
    write_wav_header(audio_dump, audio_dump_rate, audio_dump_frames);
    if (audio_dump_frames >= audio_dump_rate * 60) {
        fclose(audio_dump);
        audio_dump = nullptr;
        fprintf(stderr, "[wg3d] audio dump closed (%u frames at %u Hz)\n", audio_dump_frames, audio_dump_rate);
    }
}

static void queue_samples(int16_t* audio_data, size_t sample_count) {
    wg3d::stats::audio_buffers++;
    int peak = 0;
    for (size_t i = 0; i < sample_count; i++) peak = std::max(peak, std::abs((int)audio_data[i]));
    wg3d::stats::audio_peak = std::max<uint64_t>(wg3d::stats::audio_peak.load(), (uint64_t)peak);
    wg3d::stats::audio_samples += sample_count;
    dump_audio(audio_data, sample_count);
    if (speed > 1) {
        return;  // N times too much audio per second; would only fill the queue
    }
    static std::vector<float> swap_buffer;
    static std::array<float, duplicated_input_frames * input_channels> duplicated_sample_buffer;

    size_t resampled_sample_count = sample_count + duplicated_input_frames * input_channels;
    size_t max_sample_count = std::max(resampled_sample_count, resampled_sample_count * audio_convert.len_mult);
    if (max_sample_count > swap_buffer.size()) {
        swap_buffer.resize(max_sample_count);
    }
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        swap_buffer[i] = duplicated_sample_buffer[i];
    }
    // Convert to float and swap channels (the RDRAM word swap swaps the two 16-bit samples).
    for (size_t i = 0; i < sample_count; i += input_channels) {
        swap_buffer[i + 0 + duplicated_input_frames * input_channels] = audio_data[i + 1] * (1.0f / 32768.0f);
        swap_buffer[i + 1 + duplicated_input_frames * input_channels] = audio_data[i + 0] * (1.0f / 32768.0f);
    }
    if (sample_count <= duplicated_input_frames * input_channels) {
        return;
    }
    for (size_t i = 0; i < duplicated_input_frames * input_channels; i++) {
        duplicated_sample_buffer[i] = swap_buffer[i + sample_count];
    }

    audio_convert.buf = reinterpret_cast<Uint8*>(swap_buffer.data());
    audio_convert.len = (sample_count + duplicated_input_frames * input_channels) * sizeof(swap_buffer[0]);
    if (SDL_ConvertAudio(&audio_convert) < 0) {
        throw std::runtime_error(std::string("SDL audio conversion failed: ") + SDL_GetError());
    }

    uint64_t cur_queued_microseconds = uint64_t(SDL_GetQueuedAudioSize(audio_device)) / bytes_per_frame * 1000000 / sample_rate;
    uint32_t num_bytes_to_queue = audio_convert.len_cvt - output_channels * discarded_output_frames * sizeof(swap_buffer[0]);
    float* samples_to_queue = swap_buffer.data() + output_channels * discarded_output_frames / 2;

    // Drop samples instead of letting latency build up.
    uint32_t skip_factor = cur_queued_microseconds / 100000;
    if (skip_factor != 0) {
        uint32_t skip_ratio = 1 << skip_factor;
        num_bytes_to_queue /= skip_ratio;
        for (size_t i = 0; i < num_bytes_to_queue / (output_channels * sizeof(swap_buffer[0])); i++) {
            samples_to_queue[2 * i + 0] = samples_to_queue[2 * skip_ratio * i + 0];
            samples_to_queue[2 * i + 1] = samples_to_queue[2 * skip_ratio * i + 1];
        }
    }
    float out_peak = 0.0f;
    for (size_t i = 0; i < num_bytes_to_queue / sizeof(float); i++) out_peak = std::max(out_peak, std::fabs(samples_to_queue[i]));
    wg3d::stats::audio_out_peak_milli = std::max<uint64_t>(wg3d::stats::audio_out_peak_milli.load(), (uint64_t)(out_peak * 1000));
    wg3d::stats::audio_out_bytes += num_bytes_to_queue;
    SDL_QueueAudio(audio_device, samples_to_queue, num_bytes_to_queue);
}

static size_t get_frames_remaining() {
    constexpr float buffer_offset_frames = 1.0f;
    uint64_t buffered_byte_count = SDL_GetQueuedAudioSize(audio_device);
    buffered_byte_count = buffered_byte_count * 2 * sample_rate / output_sample_rate / output_channels;
    uint32_t frames_per_vi = sample_rate / 60;
    if (buffered_byte_count > (buffer_offset_frames * bytes_per_frame * frames_per_vi)) {
        buffered_byte_count -= (buffer_offset_frames * bytes_per_frame * frames_per_vi);
    } else {
        buffered_byte_count = 0;
    }
    return static_cast<uint32_t>(buffered_byte_count / bytes_per_frame);
}

static void update_audio_converter() {
    if (SDL_BuildAudioCVT(&audio_convert, AUDIO_F32, input_channels, sample_rate, AUDIO_F32, output_channels, output_sample_rate) < 0) {
        throw std::runtime_error(std::string("SDL audio converter: ") + SDL_GetError());
    }
    discarded_output_frames = duplicated_input_frames * output_sample_rate / sample_rate;
}

static void set_frequency(uint32_t freq) {
    fprintf(stderr, "[wg3d] audio set_frequency(%u)\n", freq);
    sample_rate = freq;
    update_audio_converter();
}

static void reset_audio(uint32_t output_freq) {
    SDL_AudioSpec spec_desired{};
    spec_desired.freq = (int)output_freq;
    spec_desired.format = AUDIO_F32;
    spec_desired.channels = (Uint8)output_channels;
    spec_desired.samples = 0x100;
    SDL_AudioSpec spec_obtained{};
    audio_device = SDL_OpenAudioDevice(nullptr, false, &spec_desired, &spec_obtained, 0);
    if (audio_device == 0) {
        exit_error(std::string("SDL error opening audio device: ") + SDL_GetError());
    }
    char* default_name = nullptr;
    SDL_AudioSpec default_spec{};
    SDL_GetDefaultAudioInfo(&default_name, &default_spec, 0);
    fprintf(stderr, "[wg3d] audio: driver %s, default output \"%s\" (%d Hz, %d ch), opened %d Hz %d ch, %d of %d output devices\n",
            SDL_GetCurrentAudioDriver(), default_name ? default_name : "?", default_spec.freq, default_spec.channels,
            spec_obtained.freq, spec_obtained.channels, audio_device, SDL_GetNumAudioDevices(0));
    SDL_free(default_name);
    SDL_PauseAudioDevice(audio_device, 0);
    output_sample_rate = output_freq;
    update_audio_converter();
}

// Prints the bring-up counters every 2 seconds (chunk 3.3).
extern std::atomic<uint32_t> wg3d_fpu_flags;  // src/game/test_hooks.cpp

static void print_stats() {
    static uint64_t last_ticks = SDL_GetTicks64();
    uint64_t now = SDL_GetTicks64();
    if (now - last_ticks < 2000) {
        return;
    }
    last_ticks = now;
    const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    fprintf(stderr, "[wg3d] t=%5.1fs dls=%" PRIu64 " screens=%" PRIu64 " audtasks=%" PRIu64 " aibufs=%" PRIu64
                    " origin_changes=%" PRIu64 " VI origin=%08X width=%u status=%08X hstart=%08X vstart=%08X xs=%08X ys=%08X\n",
            now / 1000.0, wg3d::stats::display_lists.load(), wg3d::stats::screen_updates.load(),
            wg3d::stats::audio_tasks.load(), wg3d::stats::audio_buffers.load(), wg3d::stats::origin_changes.load(),
            vi->VI_ORIGIN_REG, vi->VI_WIDTH_REG, vi->VI_STATUS_REG, vi->VI_H_START_REG, vi->VI_V_START_REG,
            vi->VI_X_SCALE_REG, vi->VI_Y_SCALE_REG);
    fprintf(stderr, "[wg3d]   audio: rate=%u samples=%" PRIu64 " peak=%" PRIu64 " out_peak=%.3f out_bytes=%" PRIu64 " sdl_queued=%u bytes dev=%u status=%d\n",
            sample_rate, wg3d::stats::audio_samples.load(), wg3d::stats::audio_peak.exchange(0),
            wg3d::stats::audio_out_peak_milli.exchange(0) / 1000.0, wg3d::stats::audio_out_bytes.load(),
            audio_device ? SDL_GetQueuedAudioSize(audio_device) : 0, audio_device,
            audio_device ? (int)SDL_GetAudioDeviceStatus(audio_device) : -1);
    const uint32_t ff = wg3d_fpu_flags.load();
    fprintf(stderr, "[wg3d]   fpu flags (main game thread, sticky): 0x%02X%s%s%s%s%s\n", ff,
            ff & 0x01 ? " invalid" : "", ff & 0x02 ? " denormal-operand" : "", ff & 0x04 ? " divide-by-zero" : "",
            ff & 0x08 ? " overflow" : "", ff & 0x10 ? " underflow" : "");
}

// ---------------------------------------------------------------------------------------------
// RSP microcode lookup (docs/phase2.md 2.5). Graphics tasks go to RT64, never here.
// ---------------------------------------------------------------------------------------------
extern RspUcodeFunc aspMain;
constexpr uint32_t ASPMAIN_TEXT_VRAM = 0x8009E530;

static RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
        case M_AUDTASK:
            if ((task->t.ucode & 0x1FFFFFFF) != (ASPMAIN_TEXT_VRAM & 0x1FFFFFFF)) {
                fprintf(stderr, "[wg3d] unexpected audio ucode %08X (expected %08X)\n", (uint32_t)task->t.ucode, ASPMAIN_TEXT_VRAM);
                return nullptr;
            }
            wg3d::stats::audio_tasks++;
            // --speed: audio is muted and the game's logic never reads the mixed samples, so skip the RSP
            // audio work, which is otherwise what limits the speed. The task completes as "broke" (done).
            if (speed > 1) {
                return [](uint8_t*, uint32_t) { return RspExitReason::Broke; };
            }
            return aspMain;
        default:
            fprintf(stderr, "[wg3d] unknown RSP task type %" PRIu32 "\n", task->t.type);
            return nullptr;
    }
}

// ---------------------------------------------------------------------------------------------
// Game threads: FPU flush-to-zero (chunk 4.6).
// libultra starts every thread with FCSR.FS = 1 (osCreateThread: FPCSR_FS | FPCSR_EV), and this game
// sets FCSR to 0x01000E00, so on the N64 denormal float results are flushed to zero. The x86
// equivalent is MXCSR FTZ (bit 15) + DAZ (bit 6), which are per host thread. ultramodern calls
// get_game_thread_name on each new game thread before its entrypoint runs, so that is where it is set.
// N64Recomp's set_cop1_cs (fesetround) only changes the rounding bits, so the flags survive the game's
// FCSR writes. WG3D_FTZ=0 turns this off (for comparisons).
// ---------------------------------------------------------------------------------------------
static bool ftz_enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("WG3D_FTZ");
        return !(v && v[0] == '0');
    }();
    return enabled;
}

static std::string game_thread_name(const OSThread* t) {
    if (ftz_enabled()) {
        _mm_setcsr(_mm_getcsr() | 0x8040);  // FTZ | DAZ
    }
    return "Game Thread " + std::to_string(t->id);
}

// ---------------------------------------------------------------------------------------------
// Error handling: message box plus the trace ring (populated in --trace builds).
// ---------------------------------------------------------------------------------------------
static void message_box(const char* msg) {
    fprintf(stderr, "[wg3d] %s\n", msg);
    wg3d::trace::dump_current_thread(stderr);
    if (smoke_frames == 0) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "W.G. 3D Hockey: Recompiled", msg, window);
    }
}

// ---------------------------------------------------------------------------------------------
// Game registration
// ---------------------------------------------------------------------------------------------
extern "C" void recomp_entrypoint(uint8_t* rdram, recomp_context* ctx);
gpr get_entrypoint_address();

// Command line (chunk 3.7): wg3d.exe [--frames N] [--speed N] [--data-dir DIR] [--console] [rom]
//   --frames N      smoke-test mode: quit cleanly after N VIs of game time (since the game started); no dialogs
//   --speed N       run the game N times faster than real time, audio muted (tools/run_scenario.py)
//   --data-dir DIR  config/save/ROM-store folder instead of %APPDATA%\WG3DRecomp (keeps tests off real saves)
//   --console       also show the output in a console (5.2: the GUI build has none). Output always goes
//                   to <app folder>/logs/wg3d.log, and to stdout/stderr when a parent redirects them.

// ---------------------------------------------------------------------------------------------
// ROM selection (chunk 5.5): command line, else the stored copy, else a file picker on first run.
// ---------------------------------------------------------------------------------------------
static const char* const wrong_rom_text =
    "This build needs Wayne Gretzky's 3D Hockey (USA) V1.0\n(sha1 400aa84811f1f2f6c62c756b43b54d534d5a5ec8; .z64, .v64 or .n64).";

// Validates and stores the ROM (the runtime copies it into the app folder). Empty string on success;
// otherwise a short message for the player (the path goes to the log only).
static std::string try_select_rom(const std::filesystem::path& rom_path) {
    std::string err;
    switch (recomp::select_rom(rom_path, game_id)) {
        case recomp::RomValidationError::Good:
            recomp::check_all_stored_roms();
            return {};
        case recomp::RomValidationError::FailedToOpen: err = "Could not open that file."; break;
        case recomp::RomValidationError::NotARom: err = "That file is not an N64 ROM."; break;
        case recomp::RomValidationError::IncorrectVersion:
        case recomp::RomValidationError::IncorrectRom:
            err = std::string("That is not the right game or version.\n\n") + wrong_rom_text;
            break;
        default: err = "That ROM could not be validated."; break;
    }
    fprintf(stderr, "[wg3d] ROM rejected: %s\n", rom_path.string().c_str());
    return err;
}

// Two-button message box; true if the first button was pressed.
static bool ask(SDL_MessageBoxFlags kind, const std::string& text, const char* yes, const char* no) {
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, yes },
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, no },
    };
    const SDL_MessageBoxData data{ (Uint32)kind, nullptr, "W.G. 3D Hockey: Recompiled", text.c_str(),
                                   SDL_arraysize(buttons), buttons, nullptr };
    int pressed = 0;
    return SDL_ShowMessageBox(&data, &pressed) == 0 && pressed == 1;
}

// First run without a ROM: explain, open a file picker, validate; repeat until a good ROM or the
// player gives up. Returns false if they quit.
static bool pick_rom() {
    std::string prompt = std::string("Choose your ROM to play.\n\n") + wrong_rom_text;
    if (!ask(SDL_MESSAGEBOX_INFORMATION, prompt, "Choose ROM...", "Quit")) {
        return false;
    }
    if (NFD_Init() != NFD_OKAY) {
        exit_error(std::string("Could not open a file dialog: ") + NFD_GetError() +
                   "\nRun once as: wg3d.exe path\\to\\rom.z64");
    }
    const nfdu8filteritem_t filters[] = { { "N64 ROM", "z64,v64,n64" } };
    bool chosen = false;
    while (!chosen) {
        nfdu8char_t* out = nullptr;
        nfdresult_t r = NFD_OpenDialogU8(&out, filters, 1, nullptr);
        if (r == NFD_CANCEL) {
            break;
        }
        if (r != NFD_OKAY) {
            std::string err = NFD_GetError();
            NFD_Quit();
            exit_error("File dialog error: " + err);
        }
        std::filesystem::path rom_path{ std::u8string(reinterpret_cast<const char8_t*>(out)) };
        NFD_FreePathU8(out);
        fprintf(stderr, "[wg3d] ROM picked: %s\n", rom_path.string().c_str());
        std::string err = try_select_rom(rom_path);
        if (err.empty()) {
            chosen = true;
        } else if (!ask(SDL_MESSAGEBOX_WARNING, err, "Try again", "Quit")) {
            break;
        }
    }
    NFD_Quit();
    return chosen;
}

static std::filesystem::path app_folder_path() {
    if (!data_dir_override.empty()) {
        return data_dir_override;
    }
#ifdef _WIN32
    if (const char* appdata = std::getenv("APPDATA")) {
        return std::filesystem::path(appdata) / "WG3DRecomp";
    }
#endif
    return std::filesystem::current_path() / "WG3DRecomp";
}

int main(int argc, char** argv) {
    std::string rom_arg;
    bool want_console = false;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--frames" && i + 1 < argc) {
            smoke_frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--speed" && i + 1 < argc) {
            speed = std::max(1u, (uint32_t)std::strtoul(argv[++i], nullptr, 10));
        } else if (arg == "--data-dir" && i + 1 < argc) {
            data_dir_override = std::filesystem::absolute(argv[++i]);
        } else if (arg == "--console") {
            want_console = true;
        } else if (arg.rfind("--", 0) == 0) {
            exit_error("Unknown option: " + arg + "\nUsage: wg3d.exe [--frames N] [--speed N] [--data-dir DIR] [--console] [rom]");
        } else {
            rom_arg = arg;
        }
    }

    std::filesystem::path config_path = app_folder_path();
    std::filesystem::create_directories(config_path);
    // From here on all output also goes to logs/wg3d.log (chunk 5.2), unbuffered so nothing is lost on a crash.
    wg3d::log::init(config_path, want_console);
    wg3d::crash::install();
    wg3d::crash::set_dialogs(smoke_frames == 0);
    wg3d::trace::init();
#ifdef _WIN32
    timeBeginPeriod(1);
    SetConsoleOutputCP(CP_UTF8);
    // Same as Quest64/Zelda: sample queueing is unreliable with the directsound backend.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif
    fprintf(stderr, "[wg3d] W.G. 3D Hockey: Recompiled %s; log: %s\n", version_string.c_str(),
            (wg3d::log::dir() / "wg3d.log").string().c_str());

    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        exit_error("Invalid version string: " + version_string);
    }
    recomp::register_config_path(config_path);
    // Settings (general/graphics/window.json); before start so the renderer is created with them.
    wg3d::config::load();

    recomp::GameEntry game{};
    game.rom_hash = 0xB615F7ACDFB899B4ULL;  // XXH3-64 of the big-endian US V1.0 ROM
    game.internal_name = "W.G. 3DHOCKEY";
    game.display_name = "Wayne Gretzky's 3D Hockey";
    game.game_id = game_id;
    game.save_type = recomp::SaveType::None;  // saves go to the Controller Pak (chunk 3.6)
    game.is_enabled = true;
    game.entrypoint_address = get_entrypoint_address();
    game.entrypoint = recomp_entrypoint;
    recomp::register_game(game);

    wg3d::register_overlays();

    // ROM: from the command line (validated and stored by the runtime), else the stored copy, else a
    // file picker (not in --frames runs, which must never wait on a dialog).
    recomp::check_all_stored_roms();
    if (!rom_arg.empty()) {
        std::string err = try_select_rom(std::filesystem::path{ rom_arg });
        if (!err.empty()) {
            exit_error(err);
        }
    }
    if (!recomp::is_rom_valid(game_id)) {
        if (smoke_frames != 0) {
            exit_error("No ROM stored yet. Run once as: wg3d.exe path\\to\\rom.z64 (or .v64/.n64)");
        }
        if (!pick_rom()) {
            fprintf(stderr, "[wg3d] no ROM chosen, quitting\n");
            return EXIT_SUCCESS;
        }
    }

    SDL_InitSubSystem(SDL_INIT_AUDIO);
    reset_audio(48000);

    recomp::Configuration cfg{};
    cfg.project_version = project_version;
    cfg.rsp_callbacks = { .get_rsp_microcode = get_rsp_microcode };
    cfg.renderer_callbacks = { .create_render_context = wg3d::renderer::create_render_context };
    cfg.audio_callbacks = { .queue_samples = queue_samples, .get_frames_remaining = get_frames_remaining, .set_frequency = set_frequency };
    cfg.input_callbacks = { .poll_input = wg3d::input::poll_input, .get_input = wg3d::input::get_input,
                            .set_rumble = wg3d::input::set_rumble,
                            .get_connected_device_info = wg3d::input::get_connected_device_info };
    cfg.gfx_callbacks = { .create_gfx = create_gfx, .create_window = create_window, .update_gfx = update_gfx };
    cfg.error_handling_callbacks = { .message_box = message_box };
    cfg.threads_callbacks = { .get_game_thread_name = game_thread_name };
    // Game-time clock for input scripts, captures and the frame log (wg3d::input::script_time).
    cfg.events_callbacks = { .vi_callback = [] { wg3d::input::on_vi(); wg3d::capture::on_vi(); } };
    if (speed > 1) {
        ultramodern::set_speed_multiplier(speed);
        fprintf(stderr, "[wg3d] speed: %ux real time, audio muted\n", speed);
    }
    fprintf(stderr, "[wg3d] FPU flush-to-zero on game threads: %s\n", ftz_enabled() ? "on" : "off (WG3D_FTZ=0)");

    // No launcher UI: start the game right away. The runtime's game thread waits for this.
    recomp::start_game(game_id, "");
    recomp::start(cfg);
    wg3d::config::save_window_state();

#ifdef _WIN32
    timeEndPeriod(1);
#endif
    return EXIT_SUCCESS;
}
