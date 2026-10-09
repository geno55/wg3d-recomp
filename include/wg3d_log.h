#ifndef WG3D_LOG_H
#define WG3D_LOG_H

#include <filesystem>

// Log file (src/main/log.cpp). Everything written to stdout/stderr (ours, the runtime's,
// RT64's) also goes to <app folder>/logs/wg3d.log; the previous two sessions are kept as wg3d.1.log and
// wg3d.2.log. Output is still forwarded to the process's own stdout/stderr when those exist (a console
// build, or a parent that redirects them, like tools/run_scenario.py). The GUI build has none; with
// want_console it attaches to the parent's console (or opens one) and forwards there.
namespace wg3d::log {
    void init(const std::filesystem::path& app_folder, bool want_console);
    // Waits (briefly) until everything written so far has reached the log file. Registered with atexit;
    // the crash handler calls it too.
    void flush();
    // logs/ in the app folder; empty before init().
    std::filesystem::path dir();
}

#endif
