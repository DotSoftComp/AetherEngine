// Aether Engine — Win32 window + OpenGL 4.5 core context + input state.
#pragma once
#include "../gl/gl_api.h"

namespace ae {

struct Input {
    bool keys[256] = {};
    bool mouseButtons[3] = {};
    float mouseX = 0, mouseY = 0;
    float mouseDX = 0, mouseDY = 0;   // per-frame deltas, reset each poll
    float wheelDelta = 0;
};

// Frame style:
//   System     — the normal OS title bar + borders (dev/tools default).
//   Borderless — no OS frame; the app draws its own title bar (the editor).
//                Still fully draggable/resizable/snappable via WM_NCHITTEST.
//   Fullscreen — borderless popup covering the whole primary monitor (game).
enum class WindowChrome { System, Borderless, Fullscreen };

class Window {
public:
    bool create(const char* title, int width, int height,
                WindowChrome chrome = WindowChrome::System);
    void destroy();

    // Pumps messages, refreshes input deltas. Returns false when the window closes.
    bool poll();
    void swapBuffers();
    void setTitle(const char* title);
    void setVSync(bool enabled);

    int width() const { return width_; }
    int height() const { return height_; }
    bool wasResized() { bool r = resized_; resized_ = false; return r; }
    const Input& input() const { return input_; }
    HWND hwnd() const { return hwnd_; }

    // ---- caption/window controls (used by the custom title bar) ----
    void minimize();
    void toggleMaximize();
    void close();
    bool isMaximized() const;
    bool borderless() const { return borderless_; }

    // Borderless caption hit-test: the app (editor) supplies this so the
    // window knows which parts of its custom title bar are draggable. Given a
    // client-space point inside the title strip, return true when the point is
    // over empty caption (draggable), false when it's over a widget (menu,
    // window buttons) that must receive the click instead. Resize borders are
    // handled by the window itself.
    using CaptionHit = bool (*)(void* user, int localX, int localY);
    void setCaptionHitTest(CaptionHit fn, void* user) { captionHit_ = fn; captionUser_ = user; }

    // Message hook (e.g. Dear ImGui's Win32 handler): called first for every
    // message; a non-zero return means "consumed, skip default handling".
    using MsgHook = LRESULT (*)(HWND, UINT, WPARAM, LPARAM);
    void setMessageHook(MsgHook hook) { msgHook_ = hook; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT handleMessage(UINT msg, WPARAM wp, LPARAM lp);
    LRESULT hitTest(LPARAM lp);

    HWND hwnd_ = nullptr;
    MsgHook msgHook_ = nullptr;
    CaptionHit captionHit_ = nullptr;
    void* captionUser_ = nullptr;
    HDC hdc_ = nullptr;
    HGLRC hglrc_ = nullptr;
    int width_ = 0, height_ = 0;
    bool shouldClose_ = false;
    bool resized_ = false;
    bool borderless_ = false;
    float lastMouseX_ = 0, lastMouseY_ = 0;
    bool firstMouse_ = true;
    Input input_;
};

} // namespace ae
