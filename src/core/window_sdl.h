// Aether Engine — SDL3-backed window + OpenGL context (the portable runtime
// host window: Windows/Linux/macOS/Android). Presents the same ae::Window
// surface the runtime uses, so main_runtime.cpp and the engine are unchanged.
//
// The editor keeps its own Win32 window (core/window.h) for its custom chrome;
// these two never coexist in one binary. `Input.keys[]` stays indexed by
// Win32 virtual-key code (see core/input.h) — SDL keycodes are translated on
// the way in so input_actions.h and every keys['W'] call site are unchanged.
#pragma once
#include "input.h"

struct SDL_Window; // avoid leaking <SDL3/SDL.h> into engine TUs

namespace ae {

enum class WindowChrome { System, Borderless, Fullscreen };

class Window {
public:
    bool create(const char* title, int width, int height,
                WindowChrome chrome = WindowChrome::System);
    void destroy();

    bool poll();          // pump events, refresh input deltas; false = closed
    void swapBuffers();
    void setTitle(const char* title);
    void setVSync(bool enabled);

    int width() const { return width_; }
    int height() const { return height_; }
    bool wasResized() { bool r = resized_; resized_ = false; return r; }
    const Input& input() const { return input_; }

    // Native handle for interop (0 when none) — kept for API parity with the
    // Win32 window; the runtime doesn't use it.
    void* nativeHandle() const;

private:
    SDL_Window* window_ = nullptr;
    void* glContext_ = nullptr; // SDL_GLContext
    int width_ = 0, height_ = 0;
    bool shouldClose_ = false;
    bool resized_ = false;
    float lastMouseX_ = 0, lastMouseY_ = 0;
    bool firstMouse_ = true;
    Input input_;
};

} // namespace ae
