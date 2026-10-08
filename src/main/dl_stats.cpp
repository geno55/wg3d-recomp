// Display-list summary for bring-up (chunk 3.3): walks an F3D display list in RDRAM and counts what
// it draws, to tell "the game draws nothing" apart from "RT64 renders it wrong". Enabled with
// WG3D_DL_STATS=1; prints the first 10 gfx tasks, then every 60th.
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "wg3d_stats.h"

namespace {
    // librecomp keeps RDRAM as native-endian 32-bit words, so a word read at a 4-byte aligned
    // physical address gives the big-endian N64 word.
    uint32_t word(const uint8_t* rdram, uint32_t phys) {
        return *reinterpret_cast<const uint32_t*>(rdram + (phys & 0x7FFFFC));
    }
}

void wg3d::stats::summarize_dl(const uint8_t* rdram, uint32_t dl_addr) {
    static const bool enabled = std::getenv("WG3D_DL_STATS") != nullptr;
    static uint64_t count = 0;
    if (!enabled) {
        return;
    }
    uint64_t n = count++;
    if (n >= 10 && n % 60 != 0) {
        return;
    }

    uint32_t segments[16] = {};
    uint32_t stack[16];
    int depth = 0;
    uint32_t pc = dl_addr & 0x7FFFFF;
    std::map<uint32_t, int> cimgs;  // color image address -> commands drawn while it was bound
    uint32_t cimg = 0;
    int tris = 0, vtx_cmds = 0, fillrects = 0, texrects = 0, dl_calls = 0, cmds = 0;
    uint32_t last_fill = 0, prim = 0, env = 0, combine_hi = 0, combine_lo = 0, othermode_h = 0, othermode_l = 0;
    int timgs = 0, timgs_nonzero = 0;
    uint32_t first_timg = 0;
    auto resolve = [&](uint32_t a) { return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x7FFFFF; };

    while (cmds++ < 200000) {
        uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        pc += 8;
        uint8_t op = w0 >> 24;
        if (op == 0xB8) {  // G_ENDDL
            if (depth == 0) break;
            pc = stack[--depth];
        } else if (op == 0x06) {  // G_DL
            dl_calls++;
            if (((w0 >> 16) & 0xFF) == 0) {
                if (depth == 16) break;
                stack[depth++] = pc;
            }
            pc = resolve(w1);
        } else if (op == 0xBC) {  // G_MOVEWORD
            if ((w0 & 0xFF) == 0x06) segments[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1 & 0x7FFFFF;
        } else if (op == 0xFF) {  // G_SETCIMG
            cimg = resolve(w1);
            cimgs[cimg];
        } else if (op == 0xBF) {
            tris++; cimgs[cimg]++;
        } else if (op == 0x04) {
            vtx_cmds++;
        } else if (op == 0xF6) {
            fillrects++; cimgs[cimg]++;
        } else if (op == 0xE4 || op == 0xE5) {
            texrects++; cimgs[cimg]++;
        } else if (op == 0xF7) {
            last_fill = w1;
        } else if (op == 0xFD) {  // G_SETTIMG: does the texture source hold any data?
            uint32_t t = resolve(w1);
            if (timgs++ == 0) first_timg = t;
            bool nonzero = false;
            for (uint32_t i = 0; i < 64; i += 4) nonzero |= word(rdram, t + i) != 0;
            timgs_nonzero += nonzero;
        } else if (op == 0xFA) {
            prim = w1;
        } else if (op == 0xFB) {
            env = w1;
        } else if (op == 0xFC) {
            combine_hi = w0 & 0xFFFFFF; combine_lo = w1;
        } else if (op == 0xBA) {
            othermode_h = w1;
        } else if (op == 0xB9) {
            othermode_l = w1;
        }
    }

    std::fprintf(stderr, "[wg3d] dl#%" PRIu64 " @%08X: cmds=%d calls=%d vtx=%d tri=%d fillrect=%d texrect=%d fill=%08X cimg:",
                 n, dl_addr, cmds, dl_calls, vtx_cmds, tris, fillrects, texrects, last_fill);
    std::fprintf(stderr, " timg=%d(nonzero %d, first %06X) prim=%08X env=%08X comb=%06X:%08X omH=%08X omL=%08X",
                 timgs, timgs_nonzero, first_timg, prim, env, combine_hi, combine_lo, othermode_h, othermode_l);
    for (auto& [addr, draws] : cimgs) {
        std::fprintf(stderr, " %06X(%d)", addr, draws);
    }
    std::fputc('\n', stderr);
}

// Main color image of a display list: the SETCIMG target that receives the most draw commands
// (triangles, rectangles). Used by the frame log (chunk 4.4). Returns 0 if nothing is drawn.
uint32_t wg3d::stats::main_color_image(const uint8_t* rdram, uint32_t dl_addr) {
    uint32_t segments[16] = {};
    uint32_t stack[16];
    int depth = 0;
    uint32_t pc = dl_addr & 0x7FFFFF;
    std::map<uint32_t, int> draws;
    uint32_t cimg = 0;
    auto resolve = [&](uint32_t a) { return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x7FFFFF; };
    for (int cmds = 0; cmds < 200000; cmds++) {
        uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        pc += 8;
        uint8_t op = w0 >> 24;
        if (op == 0xB8) {  // G_ENDDL
            if (depth == 0) break;
            pc = stack[--depth];
        } else if (op == 0x06) {  // G_DL
            if (((w0 >> 16) & 0xFF) == 0) {
                if (depth == 16) break;
                stack[depth++] = pc;
            }
            pc = resolve(w1);
        } else if (op == 0xBC) {  // G_MOVEWORD (segment)
            if ((w0 & 0xFF) == 0x06) segments[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1 & 0x7FFFFF;
        } else if (op == 0xFF) {  // G_SETCIMG
            cimg = resolve(w1);
        } else if (op == 0xBF || op == 0xF6 || op == 0xE4 || op == 0xE5) {  // TRI1, FILLRECT, TEXRECT(FLIP)
            draws[cimg]++;
        }
    }
    uint32_t best = 0;
    int best_draws = 0;
    for (auto& [addr, n] : draws) {
        if (n > best_draws) {
            best = addr;
            best_draws = n;
        }
    }
    return best;
}
