#ifndef WG3D_TRACE_H
#define WG3D_TRACE_H

// Included by recompiled code when N64Recomp's trace_mode is on (tools/gen_config.py --trace, CMake
// -DWG3D_TRACE=ON). N64Recomp emits `TRACE_ENTRY()` at the start of every function and `TRACE_RETURN()`
// before every `return;`, as bare statements, so both macros expand to complete statements.
//
// Coverage (chunk 4.2): each function reports its first entry once (a static flag per function, so the
// steady-state cost is one load and branch); WG3D_COVERAGE_FILE collects the names.
// Call tracing: the per-thread ring / WG3D_TRACE_FILE stream only run when wg3d_trace_on is set
// (WG3D_TRACE=1 or WG3D_TRACE_FILE; see src/main/trace.cpp).

#ifdef __cplusplus
extern "C" {
#endif

extern int wg3d_trace_on;
void wg3d_coverage_hit(const char* func);
void wg3d_trace_entry(const char* func);
void wg3d_trace_return(const char* func);

#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() {                                                     \
    static int wg3d_cov_seen_;                                              \
    if (!wg3d_cov_seen_) { wg3d_cov_seen_ = 1; wg3d_coverage_hit(__func__); } \
    if (wg3d_trace_on) wg3d_trace_entry(__func__);                          \
}
#define TRACE_RETURN() { if (wg3d_trace_on) wg3d_trace_return(__func__); }

#endif
