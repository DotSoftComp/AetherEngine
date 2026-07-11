// Aether Engine — walks a loaded DialogueScene node-by-node, presenting each
// node with ChoicePrompt/QteEvent (or a subtitle line) and following the
// player's choice / QTE result to the next node id. This is the runtime half
// of the save/load pipeline in dialogue_scene.h.
#pragma once
#include "choice_ui.h"
#include "dialogue_scene.h"

namespace ae {

class DialoguePlayer {
public:
    void setScene(DialogueScene scene) { scene_ = std::move(scene); }
    const DialogueScene& scene() const { return scene_; }

    // (Re)starts at the scene's start node.
    void start();
    bool finished() const { return sub_ == SubState::Done; }
    // Stops immediately without finishing playback, e.g. when an editor Stop
    // interrupts a scene mid-flight; does not touch the loaded scene data.
    void resetPlayback() { sub_ = SubState::Done; current_ = nullptr; }

    // The node currently being presented (nullptr before start()/after
    // finishing). DialogueTriggerComponent watches this to drive camera cuts
    // per-node without DialoguePlayer itself needing to know about World.
    const DialogueNode* currentNode() const { return current_; }

    // Node ids entered this playthrough, in order — the data behind the
    // Detroit-style flowchart (the dialogue editor highlights the taken path).
    const std::vector<std::string>& visitedNodes() const { return visited_; }

    // Lays out, resolves input, draws — one call per frame, same idiom as
    // ChoicePrompt/QteEvent.
    void update(UI& ui, float dt, const Rect& area);

private:
    enum class SubState { Line, Choice, Qte, Done };
    void enterNode(const std::string& id);
    void drawLine(UI& ui, const Rect& area, float alpha01);

    DialogueScene scene_;
    std::vector<std::string> visited_;
    const DialogueNode* current_ = nullptr;
    SubState sub_ = SubState::Done;
    ChoicePrompt choice_;
    QteEvent qte_;
    QteResult pendingQte_ = QteResult::None;
    float lineT_ = 0.0f, lineDur_ = 1.0f;
};

} // namespace ae
