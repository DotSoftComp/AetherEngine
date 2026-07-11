// Aether Engine — animation state machines (the AnimBP-equivalent).
//
// An AnimGraph asset (assets/anim/*.json) is a set of STATES (each plays one
// glTF clip, looping, at a speed) connected by TRANSITIONS guarded by
// PARAMETER conditions ("Speed > 0.2"), with a crossfade blend time per
// transition. Parameters are the bridge from gameplay: C++ calls
// Animator::setParam, and visual scripts use the SetAnimParam / GetAnimParam
// nodes — so "walk when the character moves" is a script feeding the
// character's velocity into the graph, zero engine code.
//
// The Animator component runs a graph on its entity's Model (searched on the
// entity, then its children — the standard capsule-root + mesh-child rig),
// advancing state time, evaluating transitions, and crossfading clips through
// Model::sampleBlended. Authored in the Anim Graph editor panel, which
// highlights the active state live during Play.
#pragma once
#include "component.h"
#include "entity.h"
#include "reflect.h"
#include <map>
#include <string>
#include <vector>

namespace ae {

class Model;

struct AnimParam {
    std::string name;
    float value = 0.0f; // default
};

struct AnimState {
    std::string id;
    std::string clip;   // glTF clip name
    float speed = 1.0f;
    bool loop = true;
    float x = 0.0f, y = 0.0f; // editor canvas position
};

struct AnimTransition {
    std::string from, to;
    std::string param;
    std::string op = ">"; // > < >= <= == !=
    float value = 0.0f;
    float blend = 0.25f;  // crossfade seconds
};

struct AnimGraph {
    std::string start;
    std::vector<AnimParam> parameters;
    std::vector<AnimState> states;
    std::vector<AnimTransition> transitions;
    int stateIndex(const std::string& id) const {
        for (size_t i = 0; i < states.size(); ++i)
            if (states[i].id == id) return (int)i;
        return -1;
    }
};

bool loadAnimGraph(AnimGraph& g, const std::string& path);
bool saveAnimGraph(const AnimGraph& g, const std::string& path);
bool animConditionMet(const std::string& op, float param, float ref);

class AnimatorComponent : public Component {
public:
    std::string graphPath; // project-relative assets/anim/*.json

    const char* typeName() const override { return "Animator"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("graph", graphPath, {PropKind::AnimGraphPath, "Anim graph"});
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
    const std::string& currentState() const;
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

private:
    Model* findModel();
    void enterState(int idx);

    AnimGraph graph_;
    std::map<std::string, float> params_;
    Model* model_ = nullptr;
    int cur_ = -1, prev_ = -1;
    int curClip_ = -1, prevClip_ = -1;
    float curTime_ = 0.0f, prevTime_ = 0.0f;
    float blendT_ = 0.0f, blendDur_ = 0.0f;
    bool started_ = false;
};

} // namespace ae
