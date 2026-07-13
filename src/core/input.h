// Aether Engine — per-frame input state (platform-neutral).
//
// This is deliberately free of any windowing/OS header so gameplay and engine
// code (world, behaviors, camera rigs, input map, UI) can read input without
// pulling in a platform. The window backend (Win32, SDL3, ...) fills it each
// poll; `keys[]` is indexed by Win32 virtual-key code (0x41='A', 0x10=Shift,
// 0x1B=Esc, ...) — non-Win32 backends translate their keycodes to that space
// so `input_actions.h` name→code mapping and `keys['W']` call sites are
// backend-independent.
#pragma once

namespace ae {

struct Input {
    bool keys[256] = {};
    bool mouseButtons[3] = {};
    float mouseX = 0, mouseY = 0;
    float mouseDX = 0, mouseDY = 0; // per-frame deltas, reset each poll
    float wheelDelta = 0;
};

} // namespace ae
