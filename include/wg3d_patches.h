// Runtime functions called from N64Recomp [[patches.hook]] text (see tools/gen_config.py).
#ifndef WG3D_PATCHES_H
#define WG3D_PATCHES_H

#ifdef __cplusplus
extern "C" {
#endif

// ultramodern (scheduling.cpp): waits up to 1ms for an external (interrupt) message, delivers it,
// then switches to a higher-priority ready thread if there is one. Inserted into busy-wait loops,
// which on the N64 rely on preemption (docs/spinloops.md).
void yield_self_1ms(uint8_t* rdram);

// src/game/test_hooks.cpp: the busy-wait hook. Calls yield_self_1ms, and first ORs the calling thread's
// MXCSR exception flags into wg3d_fpu_flags (chunk 4.6: did the game produce denormals/underflow?).
// The main game thread passes through it every frame while waiting for its gfx tasks.
void wg3d_spin_yield(uint8_t* rdram);

// src/game/test_hooks.cpp (chunk 4.6): with WG3D_FIXED_SEED=<n> set, replaces the osGetTime()-derived
// value the game passes to srand() at startup ($v1 at 0x80047F50); otherwise does nothing.
void wg3d_seed_override(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
