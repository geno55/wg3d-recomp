#ifndef WG3D_STATS_H
#define WG3D_STATS_H

#include <atomic>
#include <cstdint>

namespace wg3d::stats {
    // Bring-up counters (chunk 3.3), printed periodically by main.cpp's update_gfx.
    extern std::atomic<uint64_t> display_lists;
    extern std::atomic<uint64_t> screen_updates;
    extern std::atomic<uint64_t> audio_tasks;
    extern std::atomic<uint64_t> audio_buffers;
    extern std::atomic<uint64_t> origin_changes;        // VI origin differs from the previous screen update
    extern std::atomic<uint64_t> audio_peak;            // max |sample| since the last status line
    extern std::atomic<uint64_t> audio_samples;         // total int16 samples received by queue_samples
    extern std::atomic<uint64_t> audio_out_peak_milli;  // max |float| queued to SDL x1000 since the last status line
    extern std::atomic<uint64_t> audio_out_bytes;       // total bytes queued to SDL

    // Prints a summary of a gfx task's display list when WG3D_DL_STATS is set (src/main/dl_stats.cpp).
    void summarize_dl(const uint8_t* rdram, uint32_t dl_addr);
    // The color image a display list draws most into (src/main/dl_stats.cpp; frame log, chunk 4.4).
    uint32_t main_color_image(const uint8_t* rdram, uint32_t dl_addr);
}

#endif
