// Aether Engine — XInput gamepad polling (see gamepad.h).
#define WIN32_LEAN_AND_MEAN
#include "gamepad.h"
#include <windows.h>
#include <Xinput.h>
#include <cmath>
#include <cstring>

namespace ae {

static const char* kNames[(int)PadButton::Count] = {
    "A", "B", "X", "Y", "LB", "RB", "BACK", "START", "LS", "RS",
    "DPAD_UP", "DPAD_DOWN", "DPAD_LEFT", "DPAD_RIGHT"};

const char* padButtonName(PadButton b) { return kNames[(int)b]; }

int padButtonFromName(const char* name) {
    for (int i = 0; i < (int)PadButton::Count; ++i)
        if (_stricmp(kNames[i], name) == 0) return i;
    return -1;
}

// Radial deadzone, rescaled so the usable range maps to 0..1.
static float stick(SHORT v, float deadzone) {
    float f = v / 32767.0f;
    float a = std::fabs(f);
    if (a < deadzone) return 0.0f;
    float scaled = (a - deadzone) / (1.0f - deadzone);
    return f < 0 ? -scaled : scaled;
}

GamepadState pollGamepad(int index) {
    GamepadState s;
    XINPUT_STATE xs;
    std::memset(&xs, 0, sizeof(xs));
    if (XInputGetState((DWORD)index, &xs) != ERROR_SUCCESS) return s;
    s.connected = true;

    WORD b = xs.Gamepad.wButtons;
    s.buttons[(int)PadButton::A] = (b & XINPUT_GAMEPAD_A) != 0;
    s.buttons[(int)PadButton::B] = (b & XINPUT_GAMEPAD_B) != 0;
    s.buttons[(int)PadButton::X] = (b & XINPUT_GAMEPAD_X) != 0;
    s.buttons[(int)PadButton::Y] = (b & XINPUT_GAMEPAD_Y) != 0;
    s.buttons[(int)PadButton::LB] = (b & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    s.buttons[(int)PadButton::RB] = (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    s.buttons[(int)PadButton::Back] = (b & XINPUT_GAMEPAD_BACK) != 0;
    s.buttons[(int)PadButton::Start] = (b & XINPUT_GAMEPAD_START) != 0;
    s.buttons[(int)PadButton::LS] = (b & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    s.buttons[(int)PadButton::RS] = (b & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    s.buttons[(int)PadButton::DpadUp] = (b & XINPUT_GAMEPAD_DPAD_UP) != 0;
    s.buttons[(int)PadButton::DpadDown] = (b & XINPUT_GAMEPAD_DPAD_DOWN) != 0;
    s.buttons[(int)PadButton::DpadLeft] = (b & XINPUT_GAMEPAD_DPAD_LEFT) != 0;
    s.buttons[(int)PadButton::DpadRight] = (b & XINPUT_GAMEPAD_DPAD_RIGHT) != 0;

    const float dz = 0.24f; // XInput's recommended thumb deadzone, normalized
    s.lx = stick(xs.Gamepad.sThumbLX, dz);
    s.ly = stick(xs.Gamepad.sThumbLY, dz);
    s.rx = stick(xs.Gamepad.sThumbRX, dz);
    s.ry = stick(xs.Gamepad.sThumbRY, dz);
    s.lt = xs.Gamepad.bLeftTrigger / 255.0f;
    s.rt = xs.Gamepad.bRightTrigger / 255.0f;
    return s;
}

} // namespace ae
