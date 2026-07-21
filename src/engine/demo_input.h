// Aether Engine — scripted input playback ("demos") for automated playtesting.
//
// The engine's verify loop can already prove that a scene loads, resaves and
// renders. It cannot prove that the GAME works: that walking into a door opens
// it, that a shot kills a monster, that the exit ends the level. Those need
// input, and a headless run has none.
//
// A demo is a timeline of held input, in the same shape a player would produce:
//
//   { "demo": 1, "steps": [
//       { "seconds": 2.0, "move": [0, 1], "look": [0, 0], "label": "walk out" },
//       { "seconds": 0.6, "fire": true,   "keys": "2" },
//       { "seconds": 1.0, "look": [220, 0] } ] }
//
//   seconds  how long this step is held (deterministic frames, not wall clock)
//   move     [strafe, forward] in -1..1, mapped onto the MoveX/MoveY axes
//   look     [dx, dy] mouse delta PER SECOND (yaw right / pitch down positive)
//   fire     hold the left mouse button
//   use      hold E
//   jump     hold Space
//   keys     literal keys held this step ("2" selects the shotgun)
//   label    printed when the step begins, so a run reads as a transcript
//
// Run one with:  AetherRuntime --project <dir> --demo <file.json> [--frames N]
// Combine with --verify/--screenshot to assert on the result.
#pragma once
#include "../core/input.h"
#include <string>
#include <vector>

namespace ae {

class DemoInput {
public:
    // Loads a demo timeline. Returns false (and logs) if the file is missing
    // or malformed; the caller should then fall back to real input.
    bool load(const std::string& path);
    bool valid() const { return !steps_.empty(); }

    // Total scripted duration in seconds — a runner can size its frame budget
    // from the demo rather than guessing.
    float duration() const;

    // Fills `out` with the input held at `time`, advancing by `dt` (mouse
    // deltas are per-frame, so they need the step size). Past the end of the
    // timeline everything reads as released.
    void sample(float time, float dt, Input& out);

    // True once `time` is past the end of the timeline.
    bool finished(float time) const { return time >= duration(); }

private:
    struct Step {
        float seconds = 1.0f;
        float move[2] = {0, 0};
        float look[2] = {0, 0};
        bool fire = false, use = false, jump = false;
        std::string keys;
        std::string label;
    };
    std::vector<Step> steps_;
    int lastAnnounced_ = -1;
};

} // namespace ae
