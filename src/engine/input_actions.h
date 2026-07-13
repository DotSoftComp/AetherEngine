// Aether Engine — action/axis input mapping.
//
// Gameplay never reads raw keys: it asks for named ACTIONS ("Jump", "Boost")
// and AXES ("MoveX", "MoveY"), each bound to any mix of keyboard keys, mouse
// buttons, and gamepad buttons/sticks/triggers. Bindings are data
// (assets/input.json, hand-editable, edited in Tools > Input Map), so a game
// gets rebindable controls and controller support without touching code —
// including visual scripts, via the OnAction / IsActionDown / GetAxis nodes.
//
// The World owns one InputActions and updates it every frame from the window
// Input + the polled GamepadState; gameplay reads world.actions().
//
// Key names: A-Z, 0-9, SPACE, SHIFT, CTRL, ESC, ENTER, TAB, UP, DOWN, LEFT,
// RIGHT, LMB, RMB, MMB. Pad buttons: see core/gamepad.h (A, B, X, ...,
// DPAD_RIGHT). Pad axes: LX, LY, RX, RY, LT, RT.
#pragma once
#include "../core/input.h"
#include "../core/gamepad.h"
#include <string>
#include <vector>

namespace ae {

struct ActionBinding {
    std::string name;
    std::vector<std::string> keys;       // keyboard/mouse names
    std::vector<std::string> padButtons; // PadButton names
    // runtime
    bool down = false, wasDown = false;
};

struct AxisBinding {
    std::string name;
    std::vector<std::string> posKeys; // +1 while held
    std::vector<std::string> negKeys; // -1 while held
    std::string padAxis;              // LX/LY/RX/RY/LT/RT ("" = none)
    float scale = 1.0f;
    // runtime
    float value = 0.0f;
};

class InputActions {
public:
    std::vector<ActionBinding> actions;
    std::vector<AxisBinding> axes;

    // Jump/Interact/Boost/Pause + MoveX/MoveY/LookX/LookY with sensible
    // keyboard+gamepad bindings.
    void setDefaults();
    // Loads assets/input.json when present, else installs the defaults.
    void loadOrDefaults(const std::string& path);
    bool load(const std::string& path);
    bool save(const std::string& path) const;

    // Resolves every binding for this frame (call once, before gameplay ticks).
    void update(const Input& in, const GamepadState& pad);

    bool down(const std::string& action) const;
    bool pressed(const std::string& action) const;  // this frame's edge
    bool released(const std::string& action) const;
    float axis(const std::string& name) const;      // -1..1 (keyboard) or stick

    const GamepadState& gamepad() const { return pad_; }

private:
    GamepadState pad_;
};

// "SPACE" -> VK_SPACE etc.; -1 unknown; -2/-3/-4 = LMB/RMB/MMB.
int keyCodeFromName(const std::string& name);

} // namespace ae
