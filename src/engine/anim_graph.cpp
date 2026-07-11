// Aether Engine — animation state-machine runtime (see anim_graph.h).
#include "anim_graph.h"
#include "components.h"
#include "world.h"
#include "assets.h"
#include "../render/gltf.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cmath>
#include <fstream>
#include <sstream>

namespace ae {

bool animConditionMet(const std::string& op, float p, float ref) {
    if (op == "<") return p < ref;
    if (op == ">=") return p >= ref;
    if (op == "<=") return p <= ref;
    if (op == "==") return p == ref;
    if (op == "!=") return p != ref;
    return p > ref; // default ">"
}

// ---- (de)serialization ------------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

bool saveAnimGraph(const AnimGraph& g, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"animGraph\": 1,\n  \"start\": \"" << esc(g.start) << "\",\n";
    o << "  \"parameters\": [\n";
    for (size_t i = 0; i < g.parameters.size(); ++i)
        o << "    { \"name\": \"" << esc(g.parameters[i].name)
          << "\", \"value\": " << g.parameters[i].value << " }"
          << (i + 1 < g.parameters.size() ? "," : "") << "\n";
    o << "  ],\n  \"states\": [\n";
    for (size_t i = 0; i < g.states.size(); ++i) {
        const AnimState& s = g.states[i];
        o << "    { \"id\": \"" << esc(s.id) << "\", \"clip\": \"" << esc(s.clip)
          << "\", \"speed\": " << s.speed << ", \"loop\": " << (s.loop ? "true" : "false")
          << ", \"x\": " << s.x << ", \"y\": " << s.y << " }"
          << (i + 1 < g.states.size() ? "," : "") << "\n";
    }
    o << "  ],\n  \"transitions\": [\n";
    for (size_t i = 0; i < g.transitions.size(); ++i) {
        const AnimTransition& t = g.transitions[i];
        o << "    { \"from\": \"" << esc(t.from) << "\", \"to\": \"" << esc(t.to)
          << "\", \"param\": \"" << esc(t.param) << "\", \"op\": \"" << esc(t.op)
          << "\", \"value\": " << t.value << ", \"blend\": " << t.blend << " }"
          << (i + 1 < g.transitions.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Anim] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Anim] saved %s", path.c_str());
    return f.good();
}

bool loadAnimGraph(AnimGraph& g, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Anim] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("animGraph")) {
        AE_ERROR("[Anim] malformed graph: %s", path.c_str());
        return false;
    }
    g = AnimGraph{};
    if (const std::string* s = root.string("start")) g.start = *s;
    if (const JsonValue* ps = root.find("parameters"))
        for (size_t i = 0; i < ps->size(); ++i) {
            AnimParam p;
            if (const std::string* n = (*ps)[i].string("name")) p.name = *n;
            p.value = (float)(*ps)[i].num("value", 0.0);
            g.parameters.push_back(std::move(p));
        }
    if (const JsonValue* ss = root.find("states"))
        for (size_t i = 0; i < ss->size(); ++i) {
            AnimState s;
            if (const std::string* v = (*ss)[i].string("id")) s.id = *v;
            if (const std::string* v = (*ss)[i].string("clip")) s.clip = *v;
            s.speed = (float)(*ss)[i].num("speed", 1.0);
            s.loop = (*ss)[i].flag("loop", true);
            s.x = (float)(*ss)[i].num("x", 0.0);
            s.y = (float)(*ss)[i].num("y", 0.0);
            g.states.push_back(std::move(s));
        }
    if (const JsonValue* ts = root.find("transitions"))
        for (size_t i = 0; i < ts->size(); ++i) {
            AnimTransition t;
            if (const std::string* v = (*ts)[i].string("from")) t.from = *v;
            if (const std::string* v = (*ts)[i].string("to")) t.to = *v;
            if (const std::string* v = (*ts)[i].string("param")) t.param = *v;
            if (const std::string* v = (*ts)[i].string("op")) t.op = *v;
            t.value = (float)(*ts)[i].num("value", 0.0);
            t.blend = (float)(*ts)[i].num("blend", 0.25);
            g.transitions.push_back(std::move(t));
        }
    return true;
}

// ---- Animator ----------------------------------------------------------------
static const std::string kNone = "";

const std::string& AnimatorComponent::currentState() const {
    return (cur_ >= 0 && cur_ < (int)graph_.states.size()) ? graph_.states[cur_].id : kNone;
}

void AnimatorComponent::onDeserialized(AssetLibrary& assets) {
    graph_ = AnimGraph{};
    params_.clear();
    cur_ = prev_ = -1;
    started_ = false;
    model_ = nullptr;
    if (graphPath.empty()) return;
    std::string rel = graphPath;
    if (!loadAnimGraph(graph_, assets.resolvePath(rel))) return;
    graphPath = rel;
    for (const auto& p : graph_.parameters) params_[p.name] = p.value;
}

Model* AnimatorComponent::findModel() {
    // The standard rig is a capsule root with the mesh on a child entity.
    if (auto* mc = entity().getComponent<ModelComponent>()) {
        mc->animate = false; // the animator owns sampling now
        return mc->model;
    }
    for (Entity* c : entity().children()) {
        if (auto* mc = c->getComponent<ModelComponent>()) {
            mc->animate = false;
            return mc->model;
        }
    }
    return nullptr;
}

void AnimatorComponent::enterState(int idx) {
    if (idx < 0 || idx >= (int)graph_.states.size()) return;
    prev_ = cur_;
    prevClip_ = curClip_;
    prevTime_ = curTime_;
    cur_ = idx;
    curClip_ = model_ ? model_->clipIndex(graph_.states[idx].clip) : -1;
    curTime_ = 0.0f;
    if (model_ && curClip_ < 0)
        AE_WARN("[Anim] clip '%s' not found in model (state '%s')",
                graph_.states[idx].clip.c_str(), graph_.states[idx].id.c_str());
}

void AnimatorComponent::onStart() {
    started_ = true;
    model_ = findModel();
    if (graph_.states.empty()) return;
    int start = graph_.stateIndex(graph_.start);
    if (start < 0) start = 0;
    cur_ = -1;
    enterState(start);
    blendT_ = blendDur_ = 0.0f;
    prev_ = -1;
    AE_LOG("[Anim] start state '%s'", graph_.states[start].id.c_str());
}

void AnimatorComponent::onUpdate(float dt) {
    if (!started_ || !model_ || cur_ < 0) return;
    const AnimState& s = graph_.states[cur_];
    curTime_ += dt * s.speed;
    prevTime_ += dt * (prev_ >= 0 ? graph_.states[prev_].speed : 1.0f);
    if (blendDur_ > 0.0f) blendT_ += dt;

    // Evaluate outgoing transitions in authored order; first match wins.
    for (const auto& t : graph_.transitions) {
        if (t.from != s.id) continue;
        if (!animConditionMet(t.op, param(t.param), t.value)) continue;
        int to = graph_.stateIndex(t.to);
        if (to < 0 || to == cur_) continue;
        enterState(to);
        blendT_ = 0.0f;
        blendDur_ = t.blend;
        AE_LOG("[Anim] %s -> %s", t.from.c_str(), t.to.c_str());
        break;
    }

    // Clip-local times with per-state looping.
    auto localTime = [&](int state, int clip, float time) {
        if (clip < 0) return 0.0f;
        float dur = model_->clipDuration(clip);
        if (dur <= 0.0f) return 0.0f;
        if (state >= 0 && !graph_.states[state].loop) return time < dur ? time : dur;
        return std::fmod(time, dur);
    };

    float tCur = localTime(cur_, curClip_, curTime_);
    if (blendDur_ > 0.0f && blendT_ < blendDur_ && prevClip_ >= 0) {
        float tPrev = localTime(prev_, prevClip_, prevTime_);
        model_->sampleBlended(prevClip_, tPrev, curClip_ >= 0 ? curClip_ : prevClip_, tCur,
                              blendT_ / blendDur_);
    } else if (curClip_ >= 0) {
        model_->sampleClipTime(curClip_, tCur);
    }
}

} // namespace ae
