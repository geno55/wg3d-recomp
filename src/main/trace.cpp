// Trace support for `tools/gen_config.py --trace` builds (see include/trace.h).
// Each thread keeps a ring of its most recent function entries/returns; wg3d_trace_dump() prints
// them (called on fatal errors). If WG3D_TRACE_FILE is set, every entry is also streamed to that
// file for diffing against an emulator trace (chunk 3.3).
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "trace.h"
#include "wg3d_trace.h"

namespace {
    constexpr size_t RingSize = 256;

    struct Ring {
        std::array<const char*, RingSize> names{};
        std::array<bool, RingSize> is_return{};
        size_t pos = 0;
        size_t count = 0;
        int depth = 0;
    };

    thread_local Ring ring;
    std::mutex file_mutex;
    FILE* trace_file = nullptr;
    bool trace_file_checked = false;

    FILE* get_trace_file() {
        std::lock_guard lock{ file_mutex };
        if (!trace_file_checked) {
            trace_file_checked = true;
            if (const char* path = std::getenv("WG3D_TRACE_FILE")) {
                trace_file = std::fopen(path, "w");
            }
        }
        return trace_file;
    }

    void record(const char* func, bool is_return) {
        ring.names[ring.pos] = func;
        ring.is_return[ring.pos] = is_return;
        ring.pos = (ring.pos + 1) % RingSize;
        if (ring.count < RingSize) {
            ring.count++;
        }
        if (FILE* f = get_trace_file()) {
            std::lock_guard lock{ file_mutex };
            std::fprintf(f, "%zx %*s%s %s\n", std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF,
                         ring.depth > 64 ? 64 : (ring.depth < 0 ? 0 : ring.depth), "", is_return ? "<" : ">", func);
        }
    }
}

extern "C" void wg3d_trace_entry(const char* func) {
    record(func, false);
    ring.depth++;
}

extern "C" void wg3d_trace_return(const char* func) {
    ring.depth--;
    record(func, true);
}

void wg3d::trace::dump_current_thread(FILE* out) {
    std::fprintf(out, "[wg3d] last %zu trace events on this thread (oldest first):\n", ring.count);
    size_t start = (ring.pos + RingSize - ring.count) % RingSize;
    for (size_t i = 0; i < ring.count; i++) {
        size_t idx = (start + i) % RingSize;
        std::fprintf(out, "  %s %s\n", ring.is_return[idx] ? "<" : ">", ring.names[idx]);
    }
    if (FILE* f = get_trace_file()) {
        std::fflush(f);
    }
}
