// SPDX-License-Identifier: GPL-3.0-only
// Adapted from Quest64-Recomp (https://github.com/Rainchus/Quest64-Recomp, GPL-3.0).
// RT64-backed ultramodern renderer. Adapted from Quest64-Recomp (src/main/rt64_render_context.cpp):
// same RT64 core/VI register wiring and config mapping, minus the texture-pack/mod plumbing, which this
// project doesn't use (yet). The launcher/menu draw through RT64's render hooks (src/main/ui_render.cpp).
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "hle/rt64_application.h"

#include "ultramodern/config.hpp"
#include "ultramodern/ultramodern.hpp"

#include "wg3d_crash.h"
#include "wg3d_capture.h"
#include "wg3d_frame_log.h"
#include "wg3d_input.h"
#include "wg3d_render.h"
#include "wg3d_stats.h"
#include "wg3d_ui.h"
#include "wg3d_widescreen.h"

// src/game/test_hooks.cpp: WG3D_WATCH / WG3D_POKE, run once per display list.
void wg3d_watch(uint8_t* rdram);

// Camera cuts for frame interpolation (refresh rate above the game's 30 fps). RT64 interpolates between
// every pair of matched frames; it doesn't know when the game's camera cuts (a replay starting or ending,
// a faceoff reset, a change of screen), and would show an in-between frame mixing two unrelated views.
// The game folds its camera into the projection matrix, so the camera position is the point that
// projection maps to x = y = w = 0. In play the camera moves at most ~217 units (its speed cap) and turns
// at most ~7 degrees per frame; the cuts seen (replay start, faceoff resets, the start of play) move 995 to
// 11000 units, most also turning 80-110 degrees. Such frames, and the first frame with a 3D view after one
// without, are presented without interpolation (State::skipInterpolation, patches/rt64/0001).
namespace {
    struct Camera {
        double pos[3];
        double fwd[3];
    };

    bool camera_from(const double m[4][4], Camera& c) {
        // Solve [x y z 1] * m[:, {0, 1, 3}] = 0 (Cramer's rule).
        const int cols[3] = { 0, 1, 3 };
        double a[3][3], b[3];
        for (int k = 0; k < 3; k++) {
            for (int r = 0; r < 3; r++) a[k][r] = m[r][cols[k]];
            b[k] = -m[3][cols[k]];
        }
        auto det3 = [](const double x[3][3]) {
            return x[0][0] * (x[1][1] * x[2][2] - x[1][2] * x[2][1]) - x[0][1] * (x[1][0] * x[2][2] - x[1][2] * x[2][0]) +
                   x[0][2] * (x[1][0] * x[2][1] - x[1][1] * x[2][0]);
        };
        const double d = det3(a);
        if (std::fabs(d) < 1e-12) return false;
        for (int r = 0; r < 3; r++) {
            double t[3][3];
            std::memcpy(t, a, sizeof(t));
            for (int k = 0; k < 3; k++) t[k][r] = b[k];
            c.pos[r] = det3(t) / d;
        }
        // Viewing direction: perpendicular to the planes clip x = 0 and clip y = 0.
        const double x[3] = { m[0][0], m[1][0], m[2][0] }, y[3] = { m[0][1], m[1][1], m[2][1] };
        double f[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
        const double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
        if (n < 1e-12) return false;
        for (int i = 0; i < 3; i++) c.fwd[i] = f[i] / n;
        return true;
    }

    // True if this display list's camera is a cut from the previous display list's.
    bool camera_cut(const uint8_t* rdram, uint32_t dl_addr) {
        constexpr double MaxMove = 400.0;
        constexpr double MaxTurnDegrees = 25.0;
        static bool have_prev = false;
        static Camera prev{};
        double m[4][4];
        Camera cur{};
        if (!wg3d::stats::camera_matrix(rdram, dl_addr, m) || !camera_from(m, cur)) {
            have_prev = false;  // no 3D view: whatever comes next starts fresh
            return false;
        }
        bool cut = !have_prev;
        if (have_prev) {
            const double dx = cur.pos[0] - prev.pos[0], dy = cur.pos[1] - prev.pos[1], dz = cur.pos[2] - prev.pos[2];
            const double move = std::sqrt(dx * dx + dy * dy + dz * dz);
            const double dot = std::clamp(cur.fwd[0] * prev.fwd[0] + cur.fwd[1] * prev.fwd[1] + cur.fwd[2] * prev.fwd[2], -1.0, 1.0);
            const double turn = std::acos(dot) * 180.0 / 3.14159265358979;
            if (move > MaxMove || turn > MaxTurnDegrees) {
                cut = true;
                std::fprintf(stderr, "[wg3d] camera cut at t=%.2f (moved %.0f, turned %.0f degrees): not interpolated\n",
                             wg3d::input::script_time(), move, turn);
            }
        }
        prev = cur;
        have_prev = true;
        return cut;
    }
}

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

static unsigned int MI_INTR_REG = 0;
static unsigned int DPC_START_REG = 0;
static unsigned int DPC_END_REG = 0;
static unsigned int DPC_CURRENT_REG = 0;
static unsigned int DPC_STATUS_REG = 0;
static unsigned int DPC_CLOCK_REG = 0;
static unsigned int DPC_BUFBUSY_REG = 0;
static unsigned int DPC_PIPEBUSY_REG = 0;
static unsigned int DPC_TMEM_REG = 0;

static void dummy_check_interrupts() {}

static RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Expand: return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual: return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::Original:
        default: return RT64::UserConfiguration::AspectRatio::Original;
    }
}

static RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::MSAA2X: return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X: return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X: return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::None:
        default: return RT64::UserConfiguration::Antialiasing::None;
    }
}

static RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Display: return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual: return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::Original:
        default: return RT64::UserConfiguration::RefreshRate::Original;
    }
}

static RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::On: return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto: return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
        default: return RT64::UserConfiguration::InternalColorFormat::Standard;
    }
}

static void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0 / 9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
}

static ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success: return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI: return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound: return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound: return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    fprintf(stderr, "Unhandled RT64::Application::SetupResult\n");
    std::exit(EXIT_FAILURE);
}

static ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal: return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
        default: return ultramodern::renderer::GraphicsApi::Auto;
    }
}

wg3d::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    static unsigned char dummy_rom_header[0x40];

    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#else
    appCore.window = window_handle;
#endif
    appCore.checkInterrupts = dummy_check_interrupts;
    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;
    appCore.MI_INTR_REG = &MI_INTR_REG;
    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();
    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = developer_mode;
    app->emulatorConfig.dither.postBlendNoiseNegative = true;
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12; break;
        case ultramodern::renderer::GraphicsApi::Vulkan: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan; break;
        case ultramodern::renderer::GraphicsApi::Metal: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal; break;
        case ultramodern::renderer::GraphicsApi::Auto:
        default: app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic; break;
    }

    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
}

wg3d::renderer::RT64Context::~RT64Context() = default;

void wg3d::renderer::RT64Context::send_dl(const OSTask* task) {
    wg3d::stats::display_lists++;
    wg3d::stats::summarize_dl(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    wg3d::stats::dump_view(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    wg3d::framelog::on_display_list(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    wg3d_watch(app->core.RDRAM);  // WG3D_WATCH (src/game/test_hooks.cpp)
    // Checked on every display list, so the previous camera is always the previous frame's (fast test runs
    // render only some of them).
    const bool cut = camera_cut(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
    if (!wg3d::capture::render_needed()) {
        return;  // fast test run, no capture coming up (wg3d_capture.h)
    }
    if (cut) {
        app->state->skipInterpolation = true;
    }
    app->state->rsp->reset();
    app->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
}

// ultramodern queues this on each VI *before the game starts* (with a dummy VI mode). update_screen
// presents the launcher on its own, so there is nothing to render here.
void wg3d::renderer::RT64Context::send_dummy_workload(uint32_t) {}

void wg3d::renderer::RT64Context::update_screen() {
    wg3d::stats::screen_updates++;
    static uint32_t last_origin = 0;
    uint32_t origin = ultramodern::renderer::get_vi_regs()->VI_ORIGIN_REG;
    wg3d::framelog::on_screen_update(origin);
    if (origin != last_origin) {
        last_origin = origin;
        wg3d::stats::origin_changes++;
    }
    if (!wg3d::capture::render_needed()) {
        return;
    }
    wg3d::widescreen::set_display_size(app->sharedQueueResources->swapChainWidth, app->sharedQueueResources->swapChainHeight);
    // RT64 presents only when the screen changed; the launcher (no game yet) and the menu (game paused) need a
    // present every VI to draw over. Those presents show the last game frame as it is: with interpolation on,
    // each one otherwise replayed the last frame's interpolated in-betweens, and the background behind the
    // menu jumped back and forth (State::skipPresentInterpolation, patches/rt64/0004).
    app->state->skipPresentInterpolation = wg3d::ui::visible();
    if (wg3d::ui::visible()) {
        app->state->updateScreen(app->core.decodeVI(), true);
    } else {
        app->updateScreen();
    }
}

void wg3d::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool wg3d::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config,
                                                const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }
    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }
    set_application_user_config(app.get(), new_config);
    // Discarding the framebuffers destroys RT64's render targets, interpolated ones included, which can race
    // with a present still showing one (switching the frame rate from Display back to Original crashed in
    // VIRenderer::render). Only changes to how frames are rendered need it; the frame rate and window mode
    // don't.
    const bool discard_fbs = new_config.res_option != old_config.res_option || new_config.ds_option != old_config.ds_option ||
                             new_config.ar_option != old_config.ar_option || new_config.hr_option != old_config.hr_option ||
                             new_config.msaa_option != old_config.msaa_option || new_config.hpfb_option != old_config.hpfb_option;
    app->updateUserConfig(discard_fbs);
    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void wg3d::renderer::RT64Context::enable_instant_present() {
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    app->updateEnhancementConfig();
}

uint32_t wg3d::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float wg3d::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            return 1.0f;
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

std::unique_ptr<ultramodern::renderer::RendererContext> wg3d::renderer::create_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    wg3d::crash::set_rdram(rdram);
    return std::make_unique<wg3d::renderer::RT64Context>(rdram, window_handle, developer_mode);
}
