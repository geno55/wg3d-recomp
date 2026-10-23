// Test-only hooks called from recompiled code ([[patches.hook]] entries in tools/gen_config.py).
// Each does nothing unless its environment variable is set.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <xmmintrin.h>

#include "recomp.h"
#include "wg3d_input.h"
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

// Fight meter (func_80037A04: after a check adds to the checked player's meter, the float at +0x2B0 of
// the player struct, $a3; capped at 100. A later check that finds it at 100 with the puck away from the
// pair starts a fight). Each check adds only a few points and the meter falls back, so scripted input
// never gets there:
//   WG3D_FIGHT_LOG=1    logs the meter after every check
//   WG3D_FIGHT_FORCE=1  sets it to 100, so the next qualifying check starts a fight (fight tests)
extern "C" void wg3d_fight_log(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    static const bool log = std::getenv("WG3D_FIGHT_LOG") != nullptr;
    static const bool force = std::getenv("WG3D_FIGHT_FORCE") != nullptr;
    if (force) {
        const float full = 100.0f;
        int32_t bits;
        std::memcpy(&bits, &full, sizeof(bits));
        MEM_W(0x2B0, ctx->r7) = bits;
    }
    if (log) {
        int32_t bits = MEM_W(0x2B0, ctx->r7);
        float meter;
        std::memcpy(&meter, &bits, sizeof(meter));
        std::fprintf(stderr, "[wg3d] fight meter: player struct %08X -> %.1f\n", (uint32_t)ctx->r7, meter);
    }
}

// Sticky MXCSR exception flags seen on threads passing through wg3d_spin_yield (bits 0-5: invalid,
// denormal operand, divide by zero, overflow, underflow, precision). Printed in the status line.
std::atomic<uint32_t> wg3d_fpu_flags{ 0 };

// WG3D_FIGHT_LOG=1: logs each fight as it starts (func_80037A04 calling func_8003752C), so a test can
// expect one.
extern "C" void wg3d_fight_start(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    static const bool log = std::getenv("WG3D_FIGHT_LOG") != nullptr;
    if (log) {
        std::fprintf(stderr, "[wg3d] fight starts at t=%.2f\n", wg3d::input::script_time());
    }
}

// WG3D_POKE="<t0>-<t1>:<hex addr>:<b|h|w>=<value>,...": holds the value at that address from game time t0
// to t1 (seconds), to set up a test state, e.g. a tied game going into overtime:
// WG3D_POKE=300-470:800E6AB0:h=0,300-470:800E9F8C:h=0.
static void poke(uint8_t* rdram) {
    struct Item { double t0, t1; uint32_t addr; char type; int32_t value; };
    static const std::vector<Item> items = [] {
        std::vector<Item> out;
        if (const char* spec = std::getenv("WG3D_POKE")) {
            std::stringstream ss{ spec };
            std::string part;
            while (std::getline(ss, part, ',')) {
                Item it{};
                char type = 'w';
                if (std::sscanf(part.c_str(), "%lf-%lf:%x:%c=%d", &it.t0, &it.t1, &it.addr, &type, &it.value) == 5) {
                    it.type = type;
                    out.push_back(it);
                } else {
                    std::fprintf(stderr, "[wg3d] WG3D_POKE: can't parse '%s'\n", part.c_str());
                }
            }
        }
        return out;
    }();
    const double t = wg3d::input::script_time();
    for (const Item& it : items) {
        if (t < it.t0 || t >= it.t1) continue;
        const gpr base = (int32_t)it.addr;
        switch (it.type) {
            case 'b': MEM_B(0, base) = (int8_t)it.value; break;
            case 'h': MEM_H(0, base) = (int16_t)it.value; break;
            default: MEM_W(0, base) = it.value; break;
        }
    }
}

// WG3D_WATCH="<hex addr>:<b|h|w|f>,..." (byte, halfword, word, float): logs the values, with the game
// time, whenever one of them changes (checked on every display list, i.e. once per game frame), e.g.
// both teams' goals: WG3D_WATCH=800E6AB0:h,800E9F8C:h. Also applies WG3D_POKE.
// WG3D_RDRAM_DUMP=<t>:<file>: writes RDRAM (8 MB, as big-endian bytes like the N64 sees them) to <file>
// once game time passes t seconds. For reverse engineering (finding tables by value).
static void dump_rdram(uint8_t* rdram) {
    static const char* spec = std::getenv("WG3D_RDRAM_DUMP");
    static bool done = false;
    if (!spec || done) return;
    const char* colon = std::strchr(spec, ':');
    if (!colon || wg3d::input::script_time() < std::atof(spec)) return;
    done = true;
    std::vector<uint8_t> out(0x800000);
    for (uint32_t i = 0; i < out.size(); i++) out[i] = rdram[i ^ 3];  // RDRAM is stored word-swapped
    if (FILE* f = std::fopen(colon + 1, "wb")) {
        std::fwrite(out.data(), 1, out.size(), f);
        std::fclose(f);
        std::fprintf(stderr, "[wg3d] RDRAM dumped to %s at t=%.2f\n", colon + 1, wg3d::input::script_time());
    }
}

void wg3d_watch(uint8_t* rdram) {
    poke(rdram);
    dump_rdram(rdram);
    struct Item { uint32_t addr; char type; };
    static const std::vector<Item> items = [] {
        std::vector<Item> out;
        if (const char* spec = std::getenv("WG3D_WATCH")) {
            std::stringstream ss{ spec };
            std::string part;
            while (std::getline(ss, part, ',')) {
                size_t colon = part.find(':');
                out.push_back({ (uint32_t)std::strtoul(part.substr(0, colon).c_str(), nullptr, 16),
                                colon == std::string::npos ? 'w' : part[colon + 1] });
            }
        }
        return out;
    }();
    if (items.empty()) return;
    static std::mutex mutex;
    static std::string last;
    std::string line;
    char buf[48];
    for (const Item& it : items) {
        const gpr base = (int32_t)it.addr;
        switch (it.type) {
            case 'b': std::snprintf(buf, sizeof(buf), " %08X=%d", it.addr, (int)MEM_B(0, base)); break;
            case 'h': std::snprintf(buf, sizeof(buf), " %08X=%d", it.addr, (int)MEM_H(0, base)); break;
            case 'f': {
                int32_t bits = MEM_W(0, base);
                float f;
                std::memcpy(&f, &bits, sizeof(f));
                std::snprintf(buf, sizeof(buf), " %08X=%g", it.addr, f);
                break;
            }
            default: std::snprintf(buf, sizeof(buf), " %08X=%08X", it.addr, (uint32_t)MEM_W(0, base)); break;
        }
        line += buf;
    }
    std::lock_guard lock{ mutex };
    if (line != last) {
        last = line;
        std::fprintf(stderr, "[wg3d] watch t=%.2f%s\n", wg3d::input::script_time(), line.c_str());
    }
}

// librecomp/ultramodern: what yield_self_1ms does, in two steps, and the quit flag.
namespace ultramodern {
    void wait_for_external_message_timed(uint8_t* rdram, uint32_t millis);
    void check_running_queue(uint8_t* rdram);
}
extern std::atomic_bool exited;

extern "C" void wg3d_spin_yield(uint8_t* rdram) {
    wg3d_fpu_flags.fetch_or(_mm_getcsr() & 0x3F, std::memory_order_relaxed);
    ultramodern::wait_for_external_message_timed(rdram, 1);
    // On quit, librecomp frees RDRAM once the main game thread and the event threads are joined; a game
    // thread still spinning here would then read the freed running queue (a crash on exit, seen when
    // quitting on the initials screen). Park it instead: process exit ends it.
    if (exited.load()) {
        while (true) {
            std::this_thread::sleep_for(std::chrono::hours(1));
        }
    }
    ultramodern::check_running_queue(rdram);
}
