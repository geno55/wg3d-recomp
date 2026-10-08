#ifndef WG3D_CRASH_H
#define WG3D_CRASH_H

#include <cstdint>
#include <cstdio>

namespace wg3d::crash {
    // Installs the unhandled-exception reporter (src/main/crash_handler.cpp).
    // With WG3D_DUMP_AFTER=<seconds> set, also dumps every thread's stack once after that delay;
    // WG3D_CRASH_AFTER=<seconds> crashes on purpose after that delay (tests the reporter).
    void install();
    // Whether a crash shows a dialog (on by default; off for automated --frames runs).
    void set_dialogs(bool enabled);
    // Lets crash reports translate host fault addresses inside RDRAM into N64 addresses.
    void set_rdram(uint8_t* rdram);
    // Prints the native stack of every other thread (for diagnosing hangs).
    void dump_all_threads(FILE* out);
}

#endif
