// Runtime implementations for skipped libultra functions that the runtime doesn't provide.
// (docs/libultra_coverage.md, "Remaining gaps")
#include <cstdio>

#include "recomp.h"
#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"

// s32 osPiRawReadIo(u32 devAddr, u32* data)
// libultra reads PHYS_TO_K1(osRomBase | devAddr), i.e. physical 0x10000000 | devAddr for cart ROM.
// The game's boot function (0x80003480) reads 16 words at devAddr 0x00FFB000, past the end of the
// 8MB ROM. On hardware that is PI open bus: each 16-bit halfword reads back the low 16 bits of its
// own address. The game never reads the result, so the exact values don't matter.
extern "C" void osPiRawReadIo_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t dev_addr = (uint32_t)ctx->r4;
    gpr data_ptr = ctx->r5;
    uint32_t physical = recomp::rom_base | (dev_addr & 0x0FFFFFFF);
    uint32_t offset = physical - recomp::rom_base;

    std::span<const uint8_t> rom = recomp::get_rom();
    uint32_t value;
    if (offset + 4 <= rom.size()) {
        value = ((uint32_t)rom[offset] << 24) | ((uint32_t)rom[offset + 1] << 16) |
                ((uint32_t)rom[offset + 2] << 8) | (uint32_t)rom[offset + 3];
    } else {
        // PI open bus: halfwords mirror the low 16 bits of their own address.
        value = ((physical & 0xFFFF) << 16) | ((physical + 2) & 0xFFFF);
        static int logged = 0;
        if (logged < 16) {
            logged++;
            fprintf(stderr, "[wg3d] osPiRawReadIo past ROM end: devAddr %08X -> open bus %08X\n", dev_addr, value);
        }
    }
    MEM_W(0, data_ptr) = (int32_t)value;
    ctx->r2 = 0;
}
