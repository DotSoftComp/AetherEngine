#include "window_sdl.h"
#include "log.h"
#include "../gl/gl_api.h"
#include <SDL3/SDL.h>
#include <cstdio>

namespace ae {

// SDL keycode -> Win32 virtual-key index, so Input.keys[] stays in the VK space
// the rest of the engine (input_actions.h, keys['W'], keys[0x10]) expects.
static int vkFromSDL(SDL_Keycode k) {
    if (k >= SDLK_A && k <= SDLK_Z) return 'A' + (k - SDLK_A);       // 0x41..0x5A
    if (k >= SDLK_0 && k <= SDLK_9) return '0' + (k - SDLK_0);       // 0x30..0x39
    switch (k) {
    case SDLK_SPACE:  return 0x20; // VK_SPACE
    case SDLK_RETURN: return 0x0D; // VK_RETURN
    case SDLK_ESCAPE: return 0x1B; // VK_ESCAPE
    case SDLK_TAB:    return 0x09; // VK_TAB
    case SDLK_BACKSPACE: return 0x08;
    case SDLK_DELETE: return 0x2E;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 0x10; // VK_SHIFT
    case SDLK_LCTRL:  case SDLK_RCTRL:  return 0x11; // VK_CONTROL
    case SDLK_LALT:   case SDLK_RALT:   return 0x12; // VK_MENU
    case SDLK_LEFT:  return 0x25;
    case SDLK_UP:    return 0x26;
    case SDLK_RIGHT: return 0x27;
    case SDLK_DOWN:  return 0x28;
    case SDLK_F1: case SDLK_F2: case SDLK_F3: case SDLK_F4: case SDLK_F5:
    case SDLK_F6: case SDLK_F7: case SDLK_F8: case SDLK_F9: case SDLK_F10:
    case SDLK_F11: case SDLK_F12:
        return 0x70 + (int)(k - SDLK_F1); // VK_F1..VK_F12
    default: return -1;
    }
}

bool Window::create(const char* title, int width, int height, WindowChrome chrome) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "[Window] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 5);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    SDL_WindowFlags flags = SDL_WINDOW_OPENGL;
    if (chrome == WindowChrome::Fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    else flags |= SDL_WINDOW_RESIZABLE;
    if (chrome == WindowChrome::Borderless) flags |= SDL_WINDOW_BORDERLESS;

    window_ = SDL_CreateWindow(title, width, height, flags);
    if (!window_) {
        std::fprintf(stderr, "[Window] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }
    glContext_ = SDL_GL_CreateContext(window_);
    if (!glContext_) {
        std::fprintf(stderr, "[Window] SDL_GL_CreateContext (4.5 core) failed: %s\n",
                     SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, (SDL_GLContext)glContext_);

    // GL entry points come from SDL's loader on every platform.
    if (!loadGLFunctionsWith((GLLoaderProc)SDL_GL_GetProcAddress)) return false;

    int pw = width, ph = height;
    SDL_GetWindowSizeInPixels(window_, &pw, &ph); // HiDPI-correct backbuffer size
    width_ = pw;
    height_ = ph;
    return true;
}

void Window::destroy() {
    if (glContext_) { SDL_GL_DestroyContext((SDL_GLContext)glContext_); glContext_ = nullptr; }
    if (window_) { SDL_DestroyWindow(window_); window_ = nullptr; }
    SDL_Quit();
}

bool Window::poll() {
    input_.wheelDelta = 0;
    // Per-frame look deltas come from SDL's relative motion (xrel/yrel): they
    // are zero at rest and never jump on startup. (Computing deltas from
    // absolute positions spuriously swings the FPS camera when the first
    // motion event lands a frame after the cursor's real position is known.)
    input_.mouseDX = 0;
    input_.mouseDY = 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
        case SDL_EVENT_QUIT:
            shouldClose_ = true;
            break;
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_RESIZED: {
            int pw = 0, ph = 0;
            SDL_GetWindowSizeInPixels(window_, &pw, &ph);
            if (pw > 0 && ph > 0 && (pw != width_ || ph != height_)) {
                width_ = pw; height_ = ph; resized_ = true;
            }
            break;
        }
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP: {
            int vk = vkFromSDL(e.key.key);
            if (vk >= 0 && vk < 256) input_.keys[vk] = (e.type == SDL_EVENT_KEY_DOWN);
            break;
        }
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            bool down = (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
            if (e.button.button == SDL_BUTTON_LEFT) input_.mouseButtons[0] = down;
            else if (e.button.button == SDL_BUTTON_RIGHT) input_.mouseButtons[1] = down;
            else if (e.button.button == SDL_BUTTON_MIDDLE) input_.mouseButtons[2] = down;
            break;
        }
        case SDL_EVENT_MOUSE_MOTION:
            input_.mouseX = e.motion.x;   // absolute (UI hit-testing)
            input_.mouseY = e.motion.y;
            input_.mouseDX += e.motion.xrel; // relative (camera look)
            input_.mouseDY += e.motion.yrel;
            break;
        case SDL_EVENT_MOUSE_WHEEL:
            input_.wheelDelta += e.wheel.y;
            break;
        }
    }
    return !shouldClose_;
}

void Window::setMouseCapture(bool captured) {
    if (!window_ || captured == mouseCaptured_) return;
    mouseCaptured_ = captured;
    SDL_SetWindowRelativeMouseMode(window_, captured);
}

void Window::swapBuffers() { SDL_GL_SwapWindow(window_); }
void Window::setTitle(const char* title) { SDL_SetWindowTitle(window_, title); }
void Window::setVSync(bool enabled) { SDL_GL_SetSwapInterval(enabled ? 1 : 0); }
void* Window::nativeHandle() const { return window_; }

} // namespace ae
