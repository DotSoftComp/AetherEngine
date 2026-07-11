// Aether Engine — a Detroit-style interaction point: attach to any entity
// (a prop, an NPC) with a dialogue scene file; when the named "player" entity
// (default "MainCamera") comes within `radius`, the scene starts playing and
// registers itself as the World's active dialogue. Deliberately lives in
// narrative/ rather than engine/components.h — it bridges the engine layer
// into the narrative/UI layer, and engine/ stays free of that dependency.
#pragma once
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/reflect.h"
#include "../engine/assets.h"
#include "dialogue_player.h"
#include <cstdio>
#include <string>

namespace ae {

class DialogueTriggerComponent : public Component {
public:
    float radius = 3.0f;
    bool once = false;
    std::string playerEntityName = "MainCamera";
    std::string scenePath; // file this trigger plays (kept for serialization)

    DialogueTriggerComponent() = default;
    explicit DialogueTriggerComponent(const std::string& path) { loadFromFile(path); }

    const char* typeName() const override { return "DialogueTrigger"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("scene", scenePath, {PropKind::DialogueScenePath, "Scene file"});
        v.visit("radius", radius, {PropKind::Default, "Radius", 0.05f, 0.1f, 100.0f});
        v.visit("once", once, {PropKind::Default, "Once"});
        v.visit("player", playerEntityName, {PropKind::Default, "Player entity"});
    }
    void onDeserialized(AssetLibrary& assets) override {
        if (scenePath.empty()) return;
        std::string rel = scenePath;
        loadFromFile(assets.resolvePath(rel));
        scenePath = rel; // keep the portable project-relative form
    }

    bool loadFromFile(const std::string& path) {
        scenePath = path;
        DialogueScene scene;
        if (!loadDialogueScene(scene, path)) {
            std::fprintf(stderr, "[Dialogue] failed to load %s\n", path.c_str());
            return false;
        }
        player_.setScene(std::move(scene));
        return true;
    }

    DialoguePlayer& player() { return player_; }
    void reset() {
        player_.resetPlayback();
        consumed_ = false;
        lastCameraNode_ = nullptr;
        wasFinished_ = true;
        prox_ = Prox::Unknown;
    }

    void onUpdate(float) override {
        // Drive camera cuts and story-flag writes as the dialogue advances
        // node-by-node, and release the camera back to gameplay when it ends.
        const DialogueNode* node = player_.currentNode();
        bool isFinished = player_.finished();
        if (node != lastCameraNode_) {
            lastCameraNode_ = node;
            if (node && !node->cameraName.empty())
                world().requestCamera(node->cameraName, node->cameraBlend);
            if (node && !node->setFlag.empty())
                world().missions.setFlag(node->setFlag, node->setFlagValue);
        }
        if (isFinished && !wasFinished_) world().requestCamera("");
        wasFinished_ = isFinished;

        if (!isFinished || consumed_) return;
        Entity* target = world().find(playerEntityName);
        if (!target) return;
        Vec3 d = target->worldPosition() - entity().worldPosition();
        bool inside = dot(d, d) <= radius * radius;

        // Edge-triggered: fire only on the outside -> inside transition. A
        // player who *spawns* within the radius (Prox::Unknown) must walk out
        // and come back — the scene never starts "on its own" at spawn, and a
        // repeatable trigger can't re-fire while you're still standing in it.
        if (inside && prox_ == Prox::Outside) {
            player_.start();
            world().setActiveDialogue(&player_);
            if (once) consumed_ = true;
        }
        prox_ = inside ? Prox::Inside : Prox::Outside;
    }

private:
    enum class Prox { Unknown, Outside, Inside };
    DialoguePlayer player_;
    bool consumed_ = false;
    const DialogueNode* lastCameraNode_ = nullptr;
    bool wasFinished_ = true;
    Prox prox_ = Prox::Unknown;
};

} // namespace ae
