// Display-list summary for bring-up: walks an F3D display list in RDRAM and counts what
// it draws, to tell "the game draws nothing" apart from "RT64 renders it wrong". Enabled with
// WG3D_DL_STATS=1; prints the first 10 gfx tasks, then every 60th.
// WG3D_DL_VIEW=1 (every 60th task) or WG3D_DL_VIEW=<t0>-<t1> (every task in that game-time window, in
// seconds): the scissors, viewports, projection matrices and sub-display-list calls of a display list, with
// the DL address of each command (for widescreen work: what RT64's Expand sees, and what the game culls).
#include <cinttypes>
#include <cmath>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>

#include "wg3d_input.h"
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
// (triangles, rectangles). Used by the frame log. Returns 0 if nothing is drawn.
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

void wg3d::stats::dump_view(const uint8_t* rdram, uint32_t dl_addr) {
    static const char* spec = std::getenv("WG3D_DL_VIEW");
    static uint64_t count = 0;
    if (!spec) {
        return;
    }
    double t0 = 0, t1 = 0;
    if (std::sscanf(spec, "%lf-%lf", &t0, &t1) == 2) {
        const double t = wg3d::input::script_time();
        if (t < t0 || t > t1) return;
    } else if (count++ % 60 != 0) {
        return;
    }
    auto half = [&](uint32_t phys) {
        uint32_t w = word(rdram, phys & ~3u);
        return (int16_t)((phys & 2) ? (w & 0xFFFF) : (w >> 16));
    };
    uint32_t segments[16] = {};
    uint32_t stack[16];
    int depth = 0;
    uint32_t pc = dl_addr & 0x7FFFFF;
    auto resolve = [&](uint32_t a) { return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x7FFFFF; };
    std::fprintf(stderr, "[wg3d] view dl @%06X t=%.3f\n", dl_addr, wg3d::input::script_time());
    int tris_since = 0;
    for (int cmds = 0; cmds < 200000; cmds++) {
        const uint32_t at = pc;
        uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        pc += 8;
        uint8_t op = w0 >> 24;
        if (op == 0xB8) {
            if (depth == 0) break;
            pc = stack[--depth];
        } else if (op == 0x06) {
            std::fprintf(stderr, "[wg3d]   %06X dl %08X -> %06X depth %d\n", at, w1, resolve(w1), depth);
            if (((w0 >> 16) & 0xFF) == 0) {
                if (depth == 16) break;
                stack[depth++] = pc;
            }
            pc = resolve(w1);
        } else if (op == 0xBC) {
            if ((w0 & 0xFF) == 0x06) segments[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1 & 0x7FFFFF;
        } else if (op == 0xBF || op == 0xB1) {
            tris_since++;
        } else if (op == 0xED) {  // G_SETSCISSOR
            std::fprintf(stderr, "[wg3d]   %06X (tris before: %d) scissor %.2f,%.2f - %.2f,%.2f mode %u\n", at, tris_since,
                         ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0, ((w1 >> 12) & 0xFFF) / 4.0, (w1 & 0xFFF) / 4.0, (w1 >> 24) & 3);
            tris_since = 0;
        } else if (op == 0x03 && ((w0 >> 16) & 0xFF) == 0x80) {  // G_MOVEMEM viewport
            uint32_t v = resolve(w1);
            std::fprintf(stderr, "[wg3d]   %06X (tris before: %d) viewport @%06X scale %d,%d,%d trans %d,%d,%d (x %.1f-%.1f y %.1f-%.1f)\n",
                         at, tris_since, v, half(v), half(v + 2), half(v + 4), half(v + 8), half(v + 10), half(v + 12),
                         (half(v + 8) - half(v)) / 4.0, (half(v + 8) + half(v)) / 4.0,
                         (half(v + 10) - half(v + 2)) / 4.0, (half(v + 10) + half(v + 2)) / 4.0);
            tris_since = 0;
        } else if (op == 0x01) {  // G_MTX
            uint8_t params = (w0 >> 16) & 0xFF;
            if (params & 0x01) {  // G_MTX_PROJECTION
                uint32_t m = resolve(w1);
                // s15.16 fixed point: integer halves at m..m+31, fractions at m+32..
                auto elem = [&](int r, int c) {
                    int i = r * 4 + c;
                    return half(m + i * 2) + (uint16_t)half(m + 32 + i * 2) / 65536.0;
                };
                std::fprintf(stderr, "[wg3d]   %06X projection %s @%06X [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] row3 [%.3f %.3f %.3f %.3f]\n",
                             at, (params & 0x02) ? "load" : "mul", m, elem(0, 0), elem(0, 1), elem(0, 2), elem(0, 3),
                             elem(1, 0), elem(1, 1), elem(1, 2), elem(1, 3), elem(3, 0), elem(3, 1), elem(3, 2), elem(3, 3));
            }
        }
    }
}

// The game's camera: the projection matrix stack (G_MTX with G_MTX_PROJECTION; the game loads the
// perspective and multiplies its look-at onto it) as it stands at the first vertex load drawn with a
// perspective projection. Row-vector convention, as the N64 uses: clip = [x y z 1] * m.
bool wg3d::stats::camera_matrix(const uint8_t* rdram, uint32_t dl_addr, double out[4][4]) {
    auto half = [&](uint32_t phys) {
        uint32_t w = word(rdram, phys & ~3u);
        return (int16_t)((phys & 2) ? (w & 0xFFFF) : (w >> 16));
    };
    uint32_t segments[16] = {};
    uint32_t stack[16];
    int depth = 0;
    uint32_t pc = dl_addr & 0x7FFFFF;
    auto resolve = [&](uint32_t a) { return (segments[(a >> 24) & 0xF] + (a & 0xFFFFFF)) & 0x7FFFFF; };
    double proj[4][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 1 } };
    for (int cmds = 0; cmds < 200000; cmds++) {
        uint32_t w0 = word(rdram, pc), w1 = word(rdram, pc + 4);
        pc += 8;
        uint8_t op = w0 >> 24;
        if (op == 0xB8) {
            if (depth == 0) break;
            pc = stack[--depth];
        } else if (op == 0x06) {
            if (((w0 >> 16) & 0xFF) == 0) {
                if (depth == 16) break;
                stack[depth++] = pc;
            }
            pc = resolve(w1);
        } else if (op == 0xBC) {
            if ((w0 & 0xFF) == 0x06) segments[((w0 >> 8) & 0xFFFF) / 4 & 0xF] = w1 & 0x7FFFFF;
        } else if (op == 0x01 && (((w0 >> 16) & 0xFF) & 0x01)) {  // G_MTX, G_MTX_PROJECTION
            const uint32_t m = resolve(w1);
            double mtx[4][4];
            for (int i = 0; i < 16; i++) {
                const int32_t fixed = ((int32_t)half(m + i * 2) << 16) | (uint16_t)half(m + 32 + i * 2);
                mtx[i / 4][i % 4] = fixed / 65536.0;
            }
            if (((w0 >> 16) & 0xFF) & 0x02) {  // G_MTX_LOAD
                std::memcpy(proj, mtx, sizeof(proj));
            } else {  // G_MTX_MUL: new = mtx * current
                double r[4][4];
                for (int i = 0; i < 4; i++)
                    for (int j = 0; j < 4; j++) r[i][j] = mtx[i][0] * proj[0][j] + mtx[i][1] * proj[1][j] + mtx[i][2] * proj[2][j] + mtx[i][3] * proj[3][j];
                std::memcpy(proj, r, sizeof(proj));
            }
        } else if (op == 0x04) {  // G_VTX: the first geometry drawn with a perspective projection
            // w depends on position: column 3 isn't (0, 0, 0, w) as in an orthographic projection.
            const bool perspective = std::fabs(proj[0][3]) + std::fabs(proj[1][3]) + std::fabs(proj[2][3]) > 1e-6;
            if (perspective) {
                std::memcpy(out, proj, sizeof(proj));
                return true;
            }
        }
    }
    return false;
}
