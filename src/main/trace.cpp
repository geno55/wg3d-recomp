// Trace and coverage support for WG3D_TRACE builds (see include/trace.h).
//
// Coverage: every recompiled function reports its first entry once. With WG3D_COVERAGE_FILE set,
// the names are appended (and flushed) to that file as they are first reached, so the file is
// complete however the process ends.
//
// Call tracing (opt-in, it costs a ring write per call): WG3D_TRACE=1 keeps a per-thread ring of the
// last 256 entries/returns, printed on fatal errors and crashes; WG3D_TRACE_FILE=<path> also streams
// every event to that file for diffing against an emulator trace.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "trace.h"
#include "wg3d_trace.h"

extern "C" int wg3d_trace_on = 0;

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
    FILE* coverage_file = nullptr;
    size_t coverage_count = 0;

    void record(const char* func, bool is_return) {
        ring.names[ring.pos] = func;
        ring.is_return[ring.pos] = is_return;
        ring.pos = (ring.pos + 1) % RingSize;
        if (ring.count < RingSize) {
            ring.count++;
        }
        if (trace_file) {
            std::lock_guard lock{ file_mutex };
            std::fprintf(trace_file, "%zx %*s%s %s\n", std::hash<std::thread::id>{}(std::this_thread::get_id()) & 0xFFFF,
                         ring.depth > 64 ? 64 : (ring.depth < 0 ? 0 : ring.depth), "", is_return ? "<" : ">", func);
        }
    }
}

void wg3d::trace::init() {
    if (const char* path = std::getenv("WG3D_TRACE_FILE")) {
        trace_file = std::fopen(path, "w");
    }
    if (const char* path = std::getenv("WG3D_COVERAGE_FILE")) {
        coverage_file = std::fopen(path, "w");
    }
    const char* ring_env = std::getenv("WG3D_TRACE");
    wg3d_trace_on = trace_file != nullptr || (ring_env && ring_env[0] == '1');
}

extern "C" void wg3d_coverage_hit(const char* func) {
    std::lock_guard lock{ file_mutex };
    coverage_count++;
    if (coverage_file) {
        std::fprintf(coverage_file, "%s\n", func);
        std::fflush(coverage_file);
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
    if (!wg3d_trace_on) {
        return;  // normal builds, or trace builds without WG3D_TRACE: nothing recorded
    }
    std::fprintf(out, "[wg3d] last %zu trace events on this thread (oldest first):\n", ring.count);
    size_t start = (ring.pos + RingSize - ring.count) % RingSize;
    for (size_t i = 0; i < ring.count; i++) {
        size_t idx = (start + i) % RingSize;
        std::fprintf(out, "  %s %s\n", ring.is_return[idx] ? "<" : ">", ring.names[idx]);
    }
    if (trace_file) {
        std::fflush(trace_file);
    }
}
