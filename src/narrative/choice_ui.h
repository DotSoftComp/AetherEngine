// Aether Engine — Detroit: Become Human–style narrative UI: branching dialogue
// choices with a depleting timer ring, and quick-time events (tap / hold /
// mash / directional sequence). Pure presentation + input resolution; the
// dialogue/branching *logic* lives elsewhere (a future DialogueBehavior) and
// just calls begin()/tick() on these.
#pragma once
#include "../ui/ui.h"
#include <string>
#include <vector>

namespace ae {

struct ChoiceOption {
    std::string text; // short phrase, e.g. "Stay calm" — rendered upper-case
};

enum class ChoiceResult { None, Picked, TimedOut };

// A vertical stack of diamond-marked options that fly in, with an optional
// countdown ring. Resolve by mouse hover+click, or number keys 1..9.
class ChoicePrompt {
public:
    void begin(std::vector<ChoiceOption> options, float timeLimit = 0.0f); // 0 = no timer
    bool active() const { return active_; }
    int selectedIndex() const { return selected_; }

    // Lays out, resolves input, draws, all in one call (immediate-mode idiom).
    // Returns Picked/TimedOut exactly on the frame it resolves, else None.
    ChoiceResult tick(UI& ui, float dt, const Rect& area);

private:
    std::vector<ChoiceOption> options_;
    int hovered_ = -1;
    int selected_ = -1;
    float timeLimit_ = 0.0f;
    float remaining_ = 0.0f;
    float elapsed_ = 0.0f;
    bool active_ = false;
};

enum class QteType { Tap, Hold, Mash, Sequence };
enum class QteResult { None, Success, Failed };

struct QteStep {
    int key = 0; // virtual-key code: letter/digit VKs, VK_SPACE, VK_UP/DOWN/LEFT/RIGHT, ...
};

// A circular button prompt with a progress/countdown ring. Tap/Hold/Mash use
// a single-element steps list (the key to press); Sequence uses an ordered
// list (e.g. arrow-key combo) shown as a row of upcoming glyphs.
class QteEvent {
public:
    void begin(QteType type, std::vector<QteStep> steps, float duration,
               float holdSeconds = 0.6f, int mashCount = 8);
    bool active() const { return active_; }

    // Returns Success/Failed exactly on the input-resolution frame; keeps
    // ticking (and returning None) for a brief flash/shake tail afterward so
    // the feedback animation gets to play before active() goes false.
    QteResult tick(UI& ui, float dt, const Rect& area);

private:
    QteType type_ = QteType::Tap;
    std::vector<QteStep> steps_;
    std::vector<bool> prevStepDown_;
    bool primed_ = false; // false for the first tick() after begin()
    size_t stepIndex_ = 0;
    float duration_ = 1.2f, remaining_ = 0.0f;
    float holdTarget_ = 0.6f, holdProgress_ = 0.0f;
    int mashTarget_ = 8, mashProgress_ = 0;
    bool active_ = false;
    bool resolved_ = false;
    float shake_ = 0.0f;  // fail feedback timer
    float flashT_ = 0.0f; // success feedback timer
};

} // namespace ae
