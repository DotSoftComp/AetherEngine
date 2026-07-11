// Aether Engine — gamepad state via XInput (Windows SDK; no vendored deps,
// same "use the OS" approach as XAudio2/WIC). pollGamepad reads a controller;
// the engine's action-mapping layer (engine/input_actions.h) consumes the
// plain struct, so everything above stays platform- and device-agnostic (and
// unit-testable with synthetic state).
#pragma once

namespace ae {

// Button order is fixed — the action map refers to these by name.
enum class PadButton {
    A = 0, B, X, Y, LB, RB, Back, Start, LS, RS,
    DpadUp, DpadDown, DpadLeft, DpadRight,
    Count
};
const char* padButtonName(PadButton b); // "A", "B", ..., "DPAD_RIGHT"
int padButtonFromName(const char* name); // -1 if unknown

struct GamepadState {
    bool connected = false;
    bool buttons[(int)PadButton::Count] = {};
    // Sticks in -1..1 (deadzone applied), triggers in 0..1.
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};

// Reads controller `index` (0-3). Cheap; call once per frame.
GamepadState pollGamepad(int index = 0);

} // namespace ae
