#pragma once
// Widescreen game patches (src/game/widescreen.cpp).

namespace wg3d::widescreen {
    // Renderer thread: the swap chain's size, from which the widening factor is derived.
    void set_display_size(unsigned width, unsigned height);
    // How much wider than 4:3 the game is drawn: 1 at 4:3 or with aspect_ratio Original, 1.33 at 16:9,
    // 1.75 at 21:9.
    float factor();
}
