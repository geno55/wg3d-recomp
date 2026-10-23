// SPDX-License-Identifier: GPL-3.0-only
// Widescreen (graphics.json aspect_ratio = Expand): game patches that let RT64 widen the 3D view.
// Hooks are [[patches.hook]] entries from GAME_HOOKS in tools/gen_config.py.
//
// RT64's Expand widens a perspective projection only if its scissor/viewport covers the framebuffer's
// whole width (rt64_projection_processor.cpp, G_EX_ASPECT_AUTO). During play the game draws the rink
// with a full-screen viewport but a scissor of (12, 10)-(308, 230), so the rink stayed 4:3.
//
// What doesn't need patching: players and the arena aren't culled by the view (the stands are drawn in
// distance bands, chosen by camera distance), so nothing pops in at the sides. Where the arena model ends
// (seen at 21:9 when the camera looks into a corner) the sides are black: the 4:3 camera never showed
// those areas, so the game has no geometry there. 2D elements (HUD, menus) keep their 4:3 positions in the
// middle of the screen; RT64 leaves rectangles alone.
#include <algorithm>
#include <atomic>
#include <cstdint>

#include "recomp.h"
#include "wg3d_config.h"
#include "wg3d_patches.h"
#include "wg3d_widescreen.h"

namespace {
    std::atomic<float> display_aspect{ 4.0f / 3.0f };
}

void wg3d::widescreen::set_display_size(unsigned width, unsigned height) {
    if (width > 0 && height > 0) {
        display_aspect.store(float(width) / float(height), std::memory_order_relaxed);
    }
}

float wg3d::widescreen::factor() {
    if (!wg3d::config::widescreen()) {
        return 1.0f;
    }
    return std::max(1.0f, display_aspect.load(std::memory_order_relaxed) / (4.0f / 3.0f));
}

// func_80001FE4, after it writes the gameplay G_SETSCISSOR to the display list ($v1 = the command).
// With Expand on, the scissor's x range becomes the full 0-320 (it is offset by the screen-shake halfwords
// at 0x800B84E8, so the margins are what is removed, not fixed values).
extern "C" void wg3d_wide_scissor(uint8_t* rdram, recomp_context* ctx) {
    if (!wg3d::config::widescreen()) {
        return;
    }
    constexpr int32_t Margin = 12 * 4;   // 10.2 fixed point
    constexpr int32_t Width = 320 * 4;
    uint32_t w0 = MEM_W(0x0, ctx->r3);
    uint32_t w1 = MEM_W(0x4, ctx->r3);
    const int32_t x0 = std::max<int32_t>(0, ((w0 >> 12) & 0xFFF) - Margin);
    const int32_t x1 = std::min<int32_t>(Width, ((w1 >> 12) & 0xFFF) + Margin);
    MEM_W(0x0, ctx->r3) = (w0 & ~0x00FFF000u) | ((uint32_t)x0 << 12);
    MEM_W(0x4, ctx->r3) = (w1 & ~0x00FFF000u) | ((uint32_t)x1 << 12);
}

// func_8006CB9C (the off-screen player indicators), after func_80086330 has projected a player to screen
// coordinates (halfwords at $sp+0xFC/0xFE). The game shows an arrow at the screen edge when x is outside
// 12-308; in widescreen a player out there can still be on screen, so x is scaled towards the centre by
// the widening factor first: the arrow then appears only when the player is outside the wide view (it is
// drawn at the 4:3 edge, as 2D elements can't go further out).
extern "C" void wg3d_wide_marker(uint8_t* rdram, recomp_context* ctx) {
    const float f = wg3d::widescreen::factor();
    if (f <= 1.0f) {
        return;
    }
    const int32_t x = MEM_H(0xFC, ctx->r29);
    MEM_H(0xFC, ctx->r29) = (int16_t)std::clamp<int32_t>(160 + (int32_t)((x - 160) / f), -0x8000, 0x7FFF);
}
