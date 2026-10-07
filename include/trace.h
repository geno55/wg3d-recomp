#ifndef WG3D_TRACE_H
#define WG3D_TRACE_H

// Included by recompiled code when N64Recomp's trace_mode is on (tools/gen_config.py --trace).
// N64Recomp emits `TRACE_ENTRY()` at the start of every function and `TRACE_RETURN()` before every
// `return;`, as bare statements, so both macros expand to complete statements.

#ifdef __cplusplus
extern "C" {
#endif

void wg3d_trace_entry(const char* func);
void wg3d_trace_return(const char* func);

#ifdef __cplusplus
}
#endif

#define TRACE_ENTRY() wg3d_trace_entry(__func__);
#define TRACE_RETURN() wg3d_trace_return(__func__);

#endif
