// Aether Engine — headless test for the action/axis input mapping.
//
// Feeds synthetic keyboard Input and synthetic GamepadState through
// InputActions and asserts action resolution, pressed/released edges, axis
// composition (keyboard overrides stick), and JSON round-trip. Exit 0 = pass.
#include "engine/input_actions.h"
#include <cstdio>
#include <cstdlib>

using namespace ae;

static int fails = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) { std::printf("[InputSmoke] FAIL: %s (line %d)\n", #cond, __LINE__); ++fails; } \
    } while (0)

int main() {
    InputActions map;
    map.setDefaults();

    Input kb;             // all keys up
    GamepadState pad;     // disconnected

    // Nothing pressed.
    map.update(kb, pad);
    CHECK(!map.down("Jump"));
    CHECK(map.axis("MoveX") == 0.0f);

    // Keyboard: SPACE -> Jump pressed edge, then held (no edge), then released.
    kb.keys[0x20] = true;
    map.update(kb, pad);
    CHECK(map.down("Jump"));
    CHECK(map.pressed("Jump"));
    map.update(kb, pad);
    CHECK(map.down("Jump"));
    CHECK(!map.pressed("Jump"));
    kb.keys[0x20] = false;
    map.update(kb, pad);
    CHECK(!map.down("Jump"));
    CHECK(map.released("Jump"));

    // Keyboard axis: D -> MoveX +1; A -> -1; both -> cancel to 0.
    kb.keys['D'] = true;
    map.update(kb, pad);
    CHECK(map.axis("MoveX") == 1.0f);
    kb.keys['A'] = true;
    map.update(kb, pad);
    CHECK(map.axis("MoveX") == 0.0f);
    kb.keys['A'] = kb.keys['D'] = false;

    // Gamepad: A button -> Jump; left stick -> MoveX; keyboard overrides stick.
    pad.connected = true;
    pad.buttons[(int)PadButton::A] = true;
    pad.lx = 0.63f;
    map.update(kb, pad);
    CHECK(map.down("Jump"));
    CHECK(map.axis("MoveX") > 0.6f && map.axis("MoveX") < 0.66f);
    kb.keys['A'] = true; // keyboard negative wins over the stick
    map.update(kb, pad);
    CHECK(map.axis("MoveX") == -1.0f);
    kb.keys['A'] = false;

    // Pad-button name round trip.
    CHECK(padButtonFromName("DPAD_LEFT") == (int)PadButton::DpadLeft);
    CHECK(padButtonFromName("nope") == -1);

    // JSON round-trip preserves bindings.
    const char* tmp = "input_smoke_map.json";
    CHECK(map.save(tmp));
    InputActions loaded;
    CHECK(loaded.load(tmp));
    CHECK(loaded.actions.size() == map.actions.size());
    CHECK(loaded.axes.size() == map.axes.size());
    loaded.update(kb, pad); // A button still held -> Jump via loaded bindings
    CHECK(loaded.down("Jump"));
    std::remove(tmp);

    std::printf("[InputSmoke] %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
