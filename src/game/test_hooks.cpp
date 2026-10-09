// Test-only hooks called from recompiled code ([[patches.hook]] entries in tools/gen_config.py).
// Each does nothing unless its environment variable is set.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <xmmintrin.h>

#include "recomp.h"
#include "wg3d_patches.h"

// WG3D_FIXED_SEED=<n>: the game seeds rand() once at startup with the low word of
// osGetTime(), so every run plays differently (as on hardware). Pinning the seed makes test runs
// repeatable, e.g. for determinism and FPU (flush-to-zero) comparisons.
extern "C" void wg3d_seed_override(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    static const char* seed = std::getenv("WG3D_FIXED_SEED");
    if (seed) {
        uint32_t value = static_cast<uint32_t>(std::strtoul(seed, nullptr, 0));
        std::fprintf(stderr, "[wg3d] rand seed 0x%08X replaced by WG3D_FIXED_SEED=0x%08X\n", (uint32_t)ctx->r3, value);
        ctx->r3 = static_cast<int32_t>(value);
    }
    // Startup diagnostic: the main game thread's MXCSR after the game's own FCSR writes
    // (FTZ = 0x8000, DAZ = 0x0040, rounding = bits 13-14).
    std::fprintf(stderr, "[wg3d] main game thread MXCSR 0x%04X\n", _mm_getcsr());
}

// Sticky MXCSR exception flags seen on threads passing through wg3d_spin_yield (bits 0-5: invalid,
// denormal operand, divide by zero, overflow, underflow, precision). Printed in the status line.
std::atomic<uint32_t> wg3d_fpu_flags{ 0 };

extern "C" void wg3d_spin_yield(uint8_t* rdram) {
    wg3d_fpu_flags.fetch_or(_mm_getcsr() & 0x3F, std::memory_order_relaxed);
    yield_self_1ms(rdram);
}
