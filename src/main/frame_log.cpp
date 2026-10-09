// Frame log for the frame-pacing analysis.
//
// WG3D_FRAME_LOG=<file> writes one line per event, both from the graphics thread (so in the order
// RT64 processes them):
//   D <t> <dl addr> <color image>   a gfx task, and the framebuffer it draws into
//   S <t> <VI origin>               a screen update (one per VI): what is being scanned out
// t is wg3d::input::script_time() (the input/capture clock). tools/frame_pacing.py turns the log
// into per-scene hold-length distributions, stale/flip-back counts and presented frame rates.
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include "wg3d_frame_log.h"
#include "wg3d_input.h"
#include "wg3d_stats.h"

namespace {
    FILE* log_file = nullptr;
    std::once_flag init_flag;

    FILE* get_log() {
        std::call_once(init_flag, [] {
            if (const char* path = std::getenv("WG3D_FRAME_LOG")) {
                log_file = std::fopen(path, "w");
            }
        });
        return log_file;
    }
}

void wg3d::framelog::on_display_list(const uint8_t* rdram, uint32_t dl_addr) {
    if (FILE* f = get_log()) {
        std::fprintf(f, "D %.4f %06X %06X\n", wg3d::input::script_time(), dl_addr,
                     wg3d::stats::main_color_image(rdram, dl_addr));
    }
}

void wg3d::framelog::on_screen_update(uint32_t vi_origin) {
    if (FILE* f = get_log()) {
        std::fprintf(f, "S %.4f %06X\n", wg3d::input::script_time(), vi_origin & 0xFFFFFF);
    }
}
