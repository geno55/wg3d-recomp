#ifndef WG3D_CAPTURE_H
#define WG3D_CAPTURE_H

struct SDL_Window;

namespace wg3d::capture {
    // Reads WG3D_CAPTURE / WG3D_CAPTURE_DIR (src/main/capture.cpp). Call after wg3d::input::init().
    void init();
    // Takes any captures that are due. Main thread, once per main-loop iteration.
    void update(SDL_Window* window);
}

#endif
