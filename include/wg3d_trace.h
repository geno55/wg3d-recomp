#ifndef WG3D_TRACE_INTERNAL_H
#define WG3D_TRACE_INTERNAL_H

#include <cstdio>

namespace wg3d::trace {
    // Prints the calling thread's recent trace events (only populated in --trace builds).
    void dump_current_thread(FILE* out);
}

#endif
