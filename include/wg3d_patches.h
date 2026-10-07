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

#ifdef __cplusplus
}
#endif

#endif
