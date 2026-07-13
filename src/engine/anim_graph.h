// Aether Engine — animation state machines (the AnimBP-equivalent).
//
// An AnimGraph asset (assets/anim/*.json) is a stack of LAYERS. Each layer is
// a set of STATES connected by TRANSITIONS guarded by PARAMETER conditions
// ("Speed > 0.2"), with a crossfade blend time per transition. A state either
// plays one glTF clip, or is a BLEND SPACE: a 1D or 2D blend tree whose
// motions (clip + position) are weighted live by parameters — walk/run by
// speed, strafing by velocity X/Y. Layers above the base layer overlay their
// pose onto the result, optionally restricted to a skeleton subtree via
// maskBone ("upper body plays Aim while legs keep running") and faded by a
// weight parameter. Parameters are the bridge from gameplay: C++ calls
// Animator::setParam, and visual scripts use the SetAnimParam / GetAnimParam
// nodes.
//
// The Animator component runs a graph on its entity's Model (searched on the
// entity, then its children — the standard capsule-root + mesh-child rig),
// sampling into the ModelComponent's per-instance ModelPose. It can also
// extract ROOT MOTION: the root bone's translation is removed from the pose
// and applied to the entity's transform instead, so locomotion follows the
// authored animation. Authored in the Anim Graph editor panel, which
// highlights the active state live during Play.
//
// JSON schema v2 (v1 files — flat states/transitions — still load):
//   { "animGraph": 2,
//     "parameters": [ { "name": "Speed", "value": 0 } ],
//     "layers": [
//       { "name": "Base", "weight": 1, "maskBone": "", "weightParam": "",
//         "start": "idle",
//         "states": [
//           { "id": "idle", "clip": "Idle", "speed": 1, "loop": true },
//           { "id": "move", "type": "blend1d", "paramX": "Speed",
//             "motions": [ { "clip": "Walk", "x": 1.5 },
//                          { "clip": "Run",  "x": 6.0 } ] },
//           { "id": "strafe", "type": "blend2d", "paramX": "VelX",
//             "paramY": "VelZ", "motions": [ { "clip": "RunF", "x": 0, "y": 1 } ] }
//         ],
//         "transitions": [ { "from": "idle", "to": "move", "param": "Speed",
//                            "op": ">", "value": 0.2, "blend": 0.25 } ] } ] }
#pragma once
#include "component.h"
#include "entity.h"
#include "reflect.h"
#include "../render/gltf.h"
#include <map>
#include <string>
#include <vector>

namespace ae {

class ModelComponent;

struct AnimParam {
    std::string name;
    float value = 0.0f; // default
};

// One clip inside a blend-space state, placed at (x[, y]) in parameter space.
struct BlendMotion {
    std::string clip;
    float x = 0.0f, y = 0.0f;
    float speed = 1.0f;
};

struct AnimState {
    std::string id;
    int type = 0;       // 0 = clip, 1 = blend1d, 2 = blend2d
    std::string clip;   // glTF clip name (type 0)
    float speed = 1.0f;
    bool loop = true;
    std::string paramX, paramY;      // blend-space inputs (type 1/2)
    std::vector<BlendMotion> motions;
    float x = 0.0f, y = 0.0f; // editor canvas position
};

struct AnimTransition {
    std::string from, to;
    std::string param;
    std::string op = ">"; // > < >= <= == !=
    float value = 0.0f;
    float blend = 0.25f;  // crossfade seconds
};

struct AnimLayer {
    std::string name = "Base";
    float weight = 1.0f;       // overlay strength (layer 0 ignores it)
    std::string weightParam;   // parameter overriding `weight` live ("" = fixed)
    std::string maskBone;      // restrict to this bone's subtree ("" = full body)
    std::string start;
    std::vector<AnimState> states;
    std::vector<AnimTransition> transitions;
    int stateIndex(const std::string& id) const {
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i].id == id) return (int)i;
        return -1;
    }
};

struct AnimGraph {
    std::vector<AnimParam> parameters;
    std::vector<AnimLayer> layers; // layer 0 = base (always present after load)
};

bool loadAnimGraph(AnimGraph& g, const std::string& path);
bool saveAnimGraph(const AnimGraph& g, const std::string& path);
bool animConditionMet(const std::string& op, float param, float ref);

class AnimatorComponent : public Component {
public:
    std::string graphPath; // project-relative assets/anim/*.json

    // Root motion: strip the root bone's horizontal translation from the pose
    // and move the entity by it instead (world-space, XZ plane; the vertical
    // channel stays in the pose). rootBone "" = auto-detect.
    bool rootMotion = false;
    std::string rootBone;

    const char* typeName() const override { return "Animator"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("graph", graphPath, {PropKind::AnimGraphPath, "Anim graph"});
        v.visit("rootMotion", rootMotion, {PropKind::Default, "Root motion"});
        v.visit("rootBone", rootBone, {PropKind::Default, "Root bone (blank = auto)"});
    }
    void onDeserialized(AssetLibrary& assets) override;
    void onStart() override;
    void onUpdate(float dt) override;

    // Gameplay/scripting surface.
    void setParam(const std::string& name, float v) { params_[name] = v; }
    float param(const std::string& name) const {
        auto it = params_.find(name);
        return it != params_.end() ? it->second : 0.0f;
    }
    const std::map<std::string, float>& params() const { return params_; }
    const AnimGraph& graph() const { return graph_; }
    const std::string& currentState(int layer = 0) const;
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

    // The pose this animator writes (null before onStart / without a model).
    ModelPose* pose() const;
    Model* model() const { return model_; }
    // The entity carrying the ModelComponent (for model->world transforms).
    Entity* modelEntity() const { return modelEntity_; }

private:
    // Per-layer state-machine runtime.
    struct LayerRT {
        int cur = -1, prev = -1;
        // Clip states: seconds. Blend spaces: normalized phase in [0, 1).
        float curTime = 0.0f, prevTime = 0.0f;
        float blendT = 0.0f, blendDur = 0.0f;
        bool wrapped = false; // looped this frame (root motion compensation)
        std::vector<float> mask;
        bool maskBuilt = false;
    };

    ModelComponent* findModel();
    void enterState(int layer, int idx);
    void advanceTime(int layer, float dt);
    // Samples layer `li`'s state `stateIdx` at its current time into `out`
    // (locals only; resets to bind first).
    void evalState(int li, int stateIdx, float time, ModelPose& out);
    // Blend-space motion weights at the layer's current parameter point.
    void motionWeights(const AnimState& s, std::vector<float>& w) const;
    float stateDuration(int li, int stateIdx) const;
    void applyRootMotion(float dt);

    AnimGraph graph_;
    std::map<std::string, float> params_;
    ModelComponent* mc_ = nullptr;
    Entity* modelEntity_ = nullptr;
    Model* model_ = nullptr;
    std::vector<LayerRT> layers_;
    // Reused per frame (no per-frame allocations): crossfade second buffer,
    // blend-space motion accumulator, overlay-layer result.
    ModelPose scratchState_, scratchMotion_, scratchLayer_;
    int rootNode_ = -1;
    bool rootPrevValid_ = false;
    Vec3 rootPrevPos_{0, 0, 0};
    Vec3 rootBind_{0, 0, 0};
    bool started_ = false;
};

} // namespace ae
