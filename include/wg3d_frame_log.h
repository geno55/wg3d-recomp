#ifndef WG3D_FRAME_LOG_H
#define WG3D_FRAME_LOG_H

#include <cstdint>

namespace wg3d::framelog {
    // WG3D_FRAME_LOG (src/main/frame_log.cpp). Both are called on the graphics thread.
    void on_display_list(const uint8_t* rdram, uint32_t dl_addr);
    void on_screen_update(uint32_t vi_origin);
}

#endif
