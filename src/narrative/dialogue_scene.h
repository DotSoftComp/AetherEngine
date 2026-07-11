// Aether Engine — a branching dialogue/QTE flowchart, Detroit: Become Human
// style: a graph of named nodes (spoken lines, timed choices, quick-time
// events) connected by string ids. Saved/loaded as human-readable JSON so
// level designers can author scenes by hand or with future editor tooling.
#pragma once
#include "choice_ui.h"
#include <string>
#include <vector>

namespace ae {

enum class NodeType { Line, Choice, Qte, End };

// A branching choice's option: display text + the node id it leads to.
struct DialogueOption {
    std::string text;
    std::string target;
};

struct DialogueNode {
    std::string id;
    NodeType type = NodeType::Line;

    // Optional cinematic camera cut for this node — the name of a camera
    // entity in the scene (see CameraComponent/World::requestCamera).
    // Empty = don't change whatever camera is currently active. Applies to
    // any node type, so a Qte can get its own dramatic angle too.
    std::string cameraName;
    float cameraBlend = 0.4f; // seconds; 0 = hard cut

    // Optional story-flag write when this node is entered — the bridge from
    // dialogue outcomes into the mission system (see MissionSystem flags).
    std::string setFlag;
    int setFlagValue = 1;

    // Node position in the editor's graph canvas (persisted layout metadata;
    // ignored by the runtime).
    float edX = 0.0f, edY = 0.0f;

    // NodeType::Line — a spoken/subtitle line, shown for `duration` then
    // advances to `next`.
    std::string speaker;
    std::string text;
    float duration = 2.0f;
    std::string next;

    // NodeType::Choice — see ChoicePrompt. `timeoutTarget` is where the scene
    // goes if the timer expires unanswered (empty = end the scene there).
    float timeLimit = 0.0f;
    std::vector<DialogueOption> options;
    std::string timeoutTarget;

    // NodeType::Qte — see QteEvent. `qteKeys` holds one key for Tap/Hold/Mash,
    // or the ordered combo for Sequence.
    QteType qteType = QteType::Tap;
    std::vector<int> qteKeys;
    float qteDuration = 1.4f;
    float qteHoldSeconds = 0.6f;
    int qteMashCount = 8;
    std::string successTarget;
    std::string failTarget;
};

struct DialogueScene {
    std::string name;
    std::string startNode;
    std::vector<DialogueNode> nodes;

    const DialogueNode* find(const std::string& id) const;
};

bool saveDialogueScene(const DialogueScene& scene, const std::string& path);
bool loadDialogueScene(DialogueScene& scene, const std::string& path);

} // namespace ae
