#ifndef WG3D_TRACE_INTERNAL_H
#define WG3D_TRACE_INTERNAL_H

#include <cstdio>

namespace wg3d::trace {
    // Reads WG3D_TRACE / WG3D_TRACE_FILE / WG3D_COVERAGE_FILE. Call at the start of main().
    void init();
    // Prints the calling thread's recent trace events (only populated in --trace builds).
    void dump_current_thread(FILE* out);
}

#endif
