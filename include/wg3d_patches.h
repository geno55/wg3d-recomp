// Runtime functions called from N64Recomp [[patches.hook]] text (see tools/gen_config.py).
#ifndef WG3D_PATCHES_H
#define WG3D_PATCHES_H

#ifdef __cplusplus
extern "C" {
#endif

// src/game/test_hooks.cpp: the busy-wait hook, inserted into busy-wait loops, which on the N64 rely on
// preemption (docs/spinloops.md). Does what ultramodern's yield_self_1ms does: waits up to 1ms for an
// external (interrupt) message, delivers it, then switches to a higher-priority ready thread if there is
// one; once the game is quitting it parks the thread instead. It also ORs the calling thread's MXCSR
// exception flags into wg3d_fpu_flags (did the game produce denormals or underflow?).
// The main game thread passes through it every frame while waiting for its gfx tasks.
void wg3d_spin_yield(uint8_t* rdram);

// src/game/test_hooks.cpp: with WG3D_FIXED_SEED=<n> set, replaces the osGetTime()-derived
// value the game passes to srand() at startup ($v1 at 0x80047F50); otherwise does nothing.
void wg3d_seed_override(uint8_t* rdram, recomp_context* ctx);
// src/game/test_hooks.cpp: after a check, logs (WG3D_FIGHT_LOG) or fills (WG3D_FIGHT_FORCE) the checked
// player's fight meter.
void wg3d_fight_log(uint8_t* rdram, recomp_context* ctx);
// src/game/test_hooks.cpp: with WG3D_FIGHT_LOG set, logs that a fight starts (func_8003752C).
void wg3d_fight_start(uint8_t* rdram, recomp_context* ctx);

#ifdef __cplusplus
}
#endif

#endif
