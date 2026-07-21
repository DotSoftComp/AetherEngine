// Aether Engine — behavior trees + perception for NPC decision-making.
//
// A BehaviorTree asset (assets/ai/*.json) is a classic tree: COMPOSITE nodes
// (Sequence, Selector, Parallel) route control to their children; DECORATORS
// (Inverter, Repeat, Cooldown) wrap one child; LEAVES act on the world
// (MoveToTarget, MoveToPoint, Wait, Log) or test it (CanSeeTarget, HasTarget,
// IsAtTarget). Each tick a node returns Running / Success / Failure. The tree
// runs on its entity, driving the sibling NavAgent and reading the sibling
// Perception, with a small typed blackboard shared across the tree.
//
// PerceptionComponent gives the entity senses: it scans the world each frame
// for entities whose name matches `targetTag`, keeping the nearest one that is
// within the sight cone (range + FOV, optional line-of-sight raycast) or the
// hearing radius. CanSeeTarget publishes that into the tree's blackboard, so a
// guard is just Selector[ Sequence[CanSeeTarget, MoveToTarget], Patrol ].
//
// JSON (nested, agent-authorable):
//   { "behaviorTree": 1,
//     "root": { "type": "Selector", "children": [
//       { "type": "Sequence", "children": [
//         { "type": "CanSeeTarget" },
//         { "type": "MoveToTarget" } ] },
//       { "type": "Wait", "time": 1.5 } ] } }
#pragma once
#include "../engine/component.h"
#include "../engine/entity.h"
#include "../engine/reflect.h"
#include "../core/math3d.h"
#include <string>
#include <vector>

namespace ae {

// ---- perception -----------------------------------------------------------
class PerceptionComponent : public Component {
public:
    std::string targetTag = "Player"; // substring matched against entity names
    float sightRange = 14.0f;
    float sightFovDeg = 100.0f;       // full cone angle around local -Z (forward)
    float hearingRange = 0.0f;        // 0 = off; senses matching entities in radius
    bool requireLineOfSight = true;   // raycast the sight line (needs physics)

    const char* typeName() const override { return "Perception"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("targetTag", targetTag, {PropKind::Default, "Target tag (name match)"});
        v.visit("sightRange", sightRange, {PropKind::Default, "Sight range", 0.1f, 0.0f, 200.0f});
        v.visit("sightFov", sightFovDeg, {PropKind::Angle, "Sight FOV", 1.0f, 0.0f, 360.0f});
        v.visit("hearingRange", hearingRange, {PropKind::Default, "Hearing range", 0.1f, 0.0f, 200.0f});
        v.visit("requireLineOfSight", requireLineOfSight, {PropKind::Default, "Require line of sight"});
    }

    void onUpdate(float dt) override;

    // Nearest currently-sensed target (null = none). Valid after onUpdate.
    Entity* target() const { return target_; }
    Vec3 lastKnownPos() const { return lastKnownPos_; }
    bool hasTarget() const { return target_ != nullptr; }

private:
    Entity* target_ = nullptr;
    Vec3 lastKnownPos_{0, 0, 0};
};

// ---- behavior tree --------------------------------------------------------
enum class BTStatus { Running, Success, Failure };

struct BTNode {
    std::string type;            // node kind (see the tick switch)
    std::string s;               // string param (log message / point key)
    float f = 0.0f;              // number param (wait seconds / repeat count)
    Vec3 v{0, 0, 0};             // vector param (MoveToPoint target)
    std::vector<int> children;   // indices into BehaviorTree::nodes
};

struct BehaviorTree {
    std::vector<BTNode> nodes;   // node 0 = root ("" tree = empty)
    bool empty() const { return nodes.empty(); }
};

bool loadBehaviorTree(BehaviorTree& t, const std::string& path);
bool saveBehaviorTree(const BehaviorTree& t, const std::string& path);

class BehaviorTreeComponent : public Component {
public:
    std::string treePath; // project-relative assets/ai/*.json

    const char* typeName() const override { return "BehaviorTree"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("tree", treePath, {PropKind::BehaviorTreePath, "Behavior tree"});
    }
    void onDeserialized(AssetLibrary& assets) override;
    void onUpdate(float dt) override;
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

    const BehaviorTree& tree() const { return tree_; }
    // Index of the leaf that ran (Running/Success) most recently — the editor
    // highlights it live during Play. -1 = none this frame.
    int activeLeaf() const { return activeLeaf_; }

    // Blackboard: written by condition leaves, read by action leaves.
    struct Blackboard {
        Entity* target = nullptr;
        Vec3 targetPos{0, 0, 0};
        bool hasTarget = false;
    };

private:
    // Per-leaf runtime state. Composites are STATELESS (re-ticked from the
    // start every frame) so conditions stay reactive — a guard drops the chase
    // the instant CanSeeTarget fails; only leaves carry state across frames.
    struct RT {
        float timer = 0.0f;    // Wait accumulator
        bool issued = false;   // MoveTo request sent
        Vec3 lastGoal{0, 0, 0};// MoveTo target it was last sent (re-issue on move)
    };
    BTStatus tick(int idx, float dt);

    BehaviorTree tree_;
    std::vector<RT> rt_;
    Blackboard bb_;
    float dt_ = 0.0f;    // current tick dt (leaves read it)
    int activeLeaf_ = -1;
};

} // namespace ae
