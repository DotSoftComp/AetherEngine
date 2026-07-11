#include "window.h"
#include <windowsx.h> // GET_X_LPARAM
#include <dwmapi.h>   // DwmExtendFrameIntoClientArea
#include <cstdio>

namespace ae {

static const char* kWndClass = "AetherEngineWindow";

// Thickness (client px) of the invisible resize border on a borderless window.
static const int kResizeBorder = 7;

LRESULT CALLBACK Window::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Window* self = (Window*)GetWindowLongPtrA(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTA* cs = (CREATESTRUCTA*)lp;
        SetWindowLongPtrA(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
    if (self) return self->handleMessage(msg, wp, lp);
    return DefWindowProcA(hwnd, msg, wp, lp);
}

// Custom-frame hit test: resize borders around the edges, then defer to the
// app's caption predicate for the title strip (draggable vs. widget), else
// client. Matches how VS Code / Unreal-style borderless windows behave.
LRESULT Window::hitTest(LPARAM lp) {
    POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
    ScreenToClient(hwnd_, &pt);
    int w = width_, h = height_;
    bool maximized = isMaximized();

    if (!maximized) {
        bool left = pt.x < kResizeBorder, right = pt.x >= w - kResizeBorder;
        bool top = pt.y < kResizeBorder, bottom = pt.y >= h - kResizeBorder;
        if (top && left) return HTTOPLEFT;
        if (top && right) return HTTOPRIGHT;
        if (bottom && left) return HTBOTTOMLEFT;
        if (bottom && right) return HTBOTTOMRIGHT;
        if (left) return HTLEFT;
        if (right) return HTRIGHT;
        if (top) return HTTOP;
        if (bottom) return HTBOTTOM;
    }
    // The app decides whether this title-strip point is empty caption.
    if (captionHit_ && captionHit_(captionUser_, pt.x, pt.y)) return HTCAPTION;
    return HTCLIENT;
}

LRESULT Window::handleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    // Give the hook (Dear ImGui) first look; it still lets most messages fall
    // through so the engine's Input stays populated (capture-gating is the
    // editor's job via io.WantCapture*).
    if (msgHook_ && msgHook_(hwnd_, msg, wp, lp)) return 0;

    // ---- borderless custom frame ----
    if (borderless_) {
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp == TRUE) {
                // Remove the standard frame. When maximized, clamp the client
                // to the monitor work area so it never covers the taskbar.
                if (isMaximized()) {
                    NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lp;
                    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
                    MONITORINFO mi{sizeof(mi)};
                    if (GetMonitorInfoA(mon, &mi)) p->rgrc[0] = mi.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST:
            return hitTest(lp);
        case WM_GETMINMAXINFO: {
            // Keep maximize within the work area (belt-and-suspenders with the
            // NCCALCSIZE clamp above; also stops covering the taskbar mid-anim).
            HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoA(mon, &mi)) {
                MINMAXINFO* mm = (MINMAXINFO*)lp;
                mm->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
                mm->ptMaxPosition.y = mi.rcWork.top - mi.rcMonitor.top;
                mm->ptMaxSize.x = mi.rcWork.right - mi.rcWork.left;
                mm->ptMaxSize.y = mi.rcWork.bottom - mi.rcWork.top;
                mm->ptMinTrackSize.x = 900;
                mm->ptMinTrackSize.y = 560;
            }
            return 0;
        }
        }
    }

    switch (msg) {
    case WM_CLOSE:
        shouldClose_ = true;
        return 0;
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (w > 0 && h > 0 && (w != width_ || h != height_)) {
            width_ = w; height_ = h; resized_ = true;
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (wp < 256) input_.keys[wp] = true;
        return 0;
    case WM_KEYUP: case WM_SYSKEYUP:
        if (wp < 256) input_.keys[wp] = false;
        return 0;
    case WM_LBUTTONDOWN: SetCapture(hwnd_); input_.mouseButtons[0] = true; return 0;
    case WM_LBUTTONUP:   ReleaseCapture(); input_.mouseButtons[0] = false; return 0;
    case WM_RBUTTONDOWN: SetCapture(hwnd_); input_.mouseButtons[1] = true; return 0;
    case WM_RBUTTONUP:   ReleaseCapture(); input_.mouseButtons[1] = false; return 0;
    case WM_MBUTTONDOWN: input_.mouseButtons[2] = true; return 0;
    case WM_MBUTTONUP:   input_.mouseButtons[2] = false; return 0;
    case WM_MOUSEMOVE:
        input_.mouseX = (float)(short)LOWORD(lp);
        input_.mouseY = (float)(short)HIWORD(lp);
        return 0;
    case WM_MOUSEWHEEL:
        input_.wheelDelta += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
        return 0;
    }
    return DefWindowProcA(hwnd_, msg, wp, lp);
}

bool Window::create(const char* title, int width, int height, WindowChrome chrome) {
    borderless_ = (chrome == WindowChrome::Borderless);
    HINSTANCE hinst = GetModuleHandleA(nullptr);

    WNDCLASSA wc = {};
    wc.style = CS_OWNDC | CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hinst;
    wc.hCursor = LoadCursorA(nullptr, (LPCSTR)IDC_ARROW);
    wc.lpszClassName = kWndClass;
    RegisterClassA(&wc);

    // First: a throwaway window/context to resolve wglCreateContextAttribsARB.
    HWND dummyWnd = CreateWindowA(kWndClass, "dummy", WS_OVERLAPPEDWINDOW,
                                  0, 0, 32, 32, nullptr, nullptr, hinst, nullptr);
    HDC dummyDC = GetDC(dummyWnd);
    PIXELFORMATDESCRIPTOR pfd = {};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    int pf = ChoosePixelFormat(dummyDC, &pfd);
    SetPixelFormat(dummyDC, pf, &pfd);
    HGLRC dummyRC = wglCreateContext(dummyDC);
    wglMakeCurrent(dummyDC, dummyRC);
    wglCreateContextAttribsARB =
        (PFNWGLCREATECONTEXTATTRIBSARB)wglGetProcAddress("wglCreateContextAttribsARB");
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(dummyRC);
    ReleaseDC(dummyWnd, dummyDC);
    DestroyWindow(dummyWnd);

    if (!wglCreateContextAttribsARB) {
        std::fprintf(stderr, "[Window] wglCreateContextAttribsARB unavailable\n");
        return false;
    }

    // Real window. Three chrome modes:
    //  - Fullscreen: WS_POPUP covering the whole primary monitor (no mode
    //    switch), used by the standalone game.
    //  - Borderless: WS_OVERLAPPEDWINDOW so the OS still gives us resize,
    //    aero-snap, minimize animation and drop shadow, but WM_NCCALCSIZE
    //    strips the visible frame so the app paints its own title bar.
    //  - System: the normal OS-decorated window.
    DWORD style;
    int x, y, winW, winH;
    if (chrome == WindowChrome::Fullscreen) {
        style = WS_POPUP;
        x = 0; y = 0;
        winW = width = GetSystemMetrics(SM_CXSCREEN);
        winH = height = GetSystemMetrics(SM_CYSCREEN);
    } else {
        style = WS_OVERLAPPEDWINDOW;
        if (borderless_) {
            // Client == window (no frame after NCCALCSIZE), so size directly.
            winW = width; winH = height;
        } else {
            RECT r = {0, 0, width, height};
            AdjustWindowRect(&r, style, FALSE);
            winW = r.right - r.left; winH = r.bottom - r.top;
        }
        x = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
        y = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;
    }

    hwnd_ = CreateWindowA(kWndClass, title, style, x, y, winW, winH,
                          nullptr, nullptr, hinst, this);
    if (!hwnd_) return false;
    hdc_ = GetDC(hwnd_);
    width_ = width; height_ = height;

    if (borderless_) {
        // Keep the DWM drop shadow on the frameless window, and force a frame
        // recalc so NCCALCSIZE runs before the first paint.
        MARGINS m{0, 0, 0, 1};
        DwmExtendFrameIntoClientArea(hwnd_, &m);
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }

    pf = ChoosePixelFormat(hdc_, &pfd);
    if (!SetPixelFormat(hdc_, pf, &pfd)) return false;

    const int attribs[] = {
        WGL_CONTEXT_MAJOR_VERSION_ARB, 4,
        WGL_CONTEXT_MINOR_VERSION_ARB, 5,
        WGL_CONTEXT_PROFILE_MASK_ARB, WGL_CONTEXT_CORE_PROFILE_BIT_ARB,
#ifndef NDEBUG
        WGL_CONTEXT_FLAGS_ARB, WGL_CONTEXT_DEBUG_BIT_ARB,
#endif
        0
    };
    hglrc_ = wglCreateContextAttribsARB(hdc_, nullptr, attribs);
    if (!hglrc_) {
        std::fprintf(stderr, "[Window] failed to create OpenGL 4.5 core context\n");
        return false;
    }
    wglMakeCurrent(hdc_, hglrc_);

    if (!loadGLFunctions()) return false;

    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    return true;
}

void Window::destroy() {
    if (hglrc_) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(hglrc_); hglrc_ = nullptr; }
    if (hdc_) { ReleaseDC(hwnd_, hdc_); hdc_ = nullptr; }
    if (hwnd_) { DestroyWindow(hwnd_); hwnd_ = nullptr; }
}

bool Window::poll() {
    input_.wheelDelta = 0;
    MSG msg;
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    if (firstMouse_) {
        lastMouseX_ = input_.mouseX;
        lastMouseY_ = input_.mouseY;
        firstMouse_ = false;
    }
    input_.mouseDX = input_.mouseX - lastMouseX_;
    input_.mouseDY = input_.mouseY - lastMouseY_;
    lastMouseX_ = input_.mouseX;
    lastMouseY_ = input_.mouseY;
    return !shouldClose_;
}

void Window::swapBuffers() { SwapBuffers(hdc_); }

void Window::setTitle(const char* title) { SetWindowTextA(hwnd_, title); }

void Window::setVSync(bool enabled) {
    if (wglSwapIntervalEXT) wglSwapIntervalEXT(enabled ? 1 : 0);
}

void Window::minimize() { ShowWindow(hwnd_, SW_MINIMIZE); }

void Window::toggleMaximize() {
    ShowWindow(hwnd_, isMaximized() ? SW_RESTORE : SW_MAXIMIZE);
}

void Window::close() { PostMessageA(hwnd_, WM_CLOSE, 0, 0); }

bool Window::isMaximized() const {
    WINDOWPLACEMENT wp{sizeof(wp)};
    return GetWindowPlacement(hwnd_, &wp) && wp.showCmd == SW_SHOWMAXIMIZED;
}

} // namespace ae
