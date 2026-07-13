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

static const char* stateTypeName(int type) {
    return type == 1 ? "blend1d" : type == 2 ? "blend2d" : "clip";
}

bool saveAnimGraph(const AnimGraph& g, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"animGraph\": 2,\n";
    o << "  \"parameters\": [\n";
    for (size_t i = 0; i < g.parameters.size(); ++i)
        o << "    { \"name\": \"" << esc(g.parameters[i].name)
          << "\", \"value\": " << g.parameters[i].value << " }"
          << (i + 1 < g.parameters.size() ? "," : "") << "\n";
    o << "  ],\n  \"layers\": [\n";
    for (size_t li = 0; li < g.layers.size(); ++li) {
        const AnimLayer& L = g.layers[li];
        o << "    {\n      \"name\": \"" << esc(L.name) << "\", \"weight\": " << L.weight
          << ", \"weightParam\": \"" << esc(L.weightParam) << "\", \"maskBone\": \""
          << esc(L.maskBone) << "\",\n      \"start\": \"" << esc(L.start) << "\",\n";
        o << "      \"states\": [\n";
        for (size_t i = 0; i < L.states.size(); ++i) {
            const AnimState& s = L.states[i];
            o << "        { \"id\": \"" << esc(s.id) << "\"";
            if (s.type != 0) o << ", \"type\": \"" << stateTypeName(s.type) << "\"";
            if (s.type == 0) o << ", \"clip\": \"" << esc(s.clip) << "\"";
            o << ", \"speed\": " << s.speed << ", \"loop\": " << (s.loop ? "true" : "false");
            if (s.type != 0) {
                o << ", \"paramX\": \"" << esc(s.paramX) << "\"";
                if (s.type == 2) o << ", \"paramY\": \"" << esc(s.paramY) << "\"";
                o << ", \"motions\": [";
                for (size_t m = 0; m < s.motions.size(); ++m) {
                    const BlendMotion& bm = s.motions[m];
                    o << (m ? ", " : "") << "{ \"clip\": \"" << esc(bm.clip)
                      << "\", \"x\": " << bm.x;
                    if (s.type == 2) o << ", \"y\": " << bm.y;
                    if (bm.speed != 1.0f) o << ", \"speed\": " << bm.speed;
                    o << " }";
                }
                o << "]";
            }
            o << ", \"x\": " << s.x << ", \"y\": " << s.y << " }"
              << (i + 1 < L.states.size() ? "," : "") << "\n";
        }
        o << "      ],\n      \"transitions\": [\n";
        for (size_t i = 0; i < L.transitions.size(); ++i) {
            const AnimTransition& t = L.transitions[i];
            o << "        { \"from\": \"" << esc(t.from) << "\", \"to\": \"" << esc(t.to)
              << "\", \"param\": \"" << esc(t.param) << "\", \"op\": \"" << esc(t.op)
              << "\", \"value\": " << t.value << ", \"blend\": " << t.blend << " }"
              << (i + 1 < L.transitions.size() ? "," : "") << "\n";
        }
        o << "      ]\n    }" << (li + 1 < g.layers.size() ? "," : "") << "\n";
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

static void loadStates(const JsonValue& parent, AnimLayer& L) {
    if (const std::string* s = parent.string("start")) L.start = *s;
    if (const JsonValue* ss = parent.find("states"))
        for (size_t i = 0; i < ss->size(); ++i) {
            const JsonValue& sj = (*ss)[i];
            AnimState s;
            if (const std::string* v = sj.string("id")) s.id = *v;
            if (const std::string* v = sj.string("type")) {
                if (*v == "blend1d") s.type = 1;
                else if (*v == "blend2d") s.type = 2;
            }
            if (const std::string* v = sj.string("clip")) s.clip = *v;
            s.speed = (float)sj.num("speed", 1.0);
            s.loop = sj.flag("loop", true);
            if (const std::string* v = sj.string("paramX")) s.paramX = *v;
            if (const std::string* v = sj.string("paramY")) s.paramY = *v;
            if (const JsonValue* ms = sj.find("motions"))
                for (size_t m = 0; m < ms->size(); ++m) {
                    BlendMotion bm;
                    if (const std::string* v = (*ms)[m].string("clip")) bm.clip = *v;
                    bm.x = (float)(*ms)[m].num("x", 0.0);
                    bm.y = (float)(*ms)[m].num("y", 0.0);
                    bm.speed = (float)(*ms)[m].num("speed", 1.0);
                    s.motions.push_back(std::move(bm));
                }
            s.x = (float)sj.num("x", 0.0);
            s.y = (float)sj.num("y", 0.0);
            L.states.push_back(std::move(s));
        }
    if (const JsonValue* ts = parent.find("transitions"))
        for (size_t i = 0; i < ts->size(); ++i) {
            const JsonValue& tj = (*ts)[i];
            AnimTransition t;
            if (const std::string* v = tj.string("from")) t.from = *v;
            if (const std::string* v = tj.string("to")) t.to = *v;
            if (const std::string* v = tj.string("param")) t.param = *v;
            if (const std::string* v = tj.string("op")) t.op = *v;
            t.value = (float)tj.num("value", 0.0);
            t.blend = (float)tj.num("blend", 0.25);
            L.transitions.push_back(std::move(t));
        }
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
    if (const JsonValue* ps = root.find("parameters"))
        for (size_t i = 0; i < ps->size(); ++i) {
            AnimParam p;
            if (const std::string* n = (*ps)[i].string("name")) p.name = *n;
            p.value = (float)(*ps)[i].num("value", 0.0);
            g.parameters.push_back(std::move(p));
        }
    if (const JsonValue* ls = root.find("layers")) {
        for (size_t i = 0; i < ls->size(); ++i) {
            const JsonValue& lj = (*ls)[i];
            AnimLayer L;
            if (const std::string* v = lj.string("name")) L.name = *v;
            L.weight = (float)lj.num("weight", 1.0);
            if (const std::string* v = lj.string("weightParam")) L.weightParam = *v;
            if (const std::string* v = lj.string("maskBone")) L.maskBone = *v;
            loadStates(lj, L);
            g.layers.push_back(std::move(L));
        }
    } else {
        // v1: flat start/states/transitions = the base layer.
        AnimLayer L;
        loadStates(root, L);
        g.layers.push_back(std::move(L));
    }
    if (g.layers.empty()) g.layers.push_back(AnimLayer{});
    return true;
}

// ---- Animator ----------------------------------------------------------------
static const std::string kNone = "";

const std::string& AnimatorComponent::currentState(int layer) const {
    if (layer < 0 || layer >= (int)layers_.size() || layer >= (int)graph_.layers.size())
        return kNone;
    int cur = layers_[layer].cur;
    const AnimLayer& L = graph_.layers[layer];
    return (cur >= 0 && cur < (int)L.states.size()) ? L.states[cur].id : kNone;
}

ModelPose* AnimatorComponent::pose() const {
    return mc_ ? &mc_->pose() : nullptr;
}

void AnimatorComponent::onDeserialized(AssetLibrary& assets) {
    graph_ = AnimGraph{};
    params_.clear();
    layers_.clear();
    started_ = false;
    mc_ = nullptr;
    modelEntity_ = nullptr;
    model_ = nullptr;
    rootNode_ = -1;
    rootPrevValid_ = false;
    if (graphPath.empty()) return;
    std::string rel = graphPath;
    if (!loadAnimGraph(graph_, assets.resolvePath(rel))) return;
    graphPath = rel;
    for (const auto& p : graph_.parameters) params_[p.name] = p.value;
}

ModelComponent* AnimatorComponent::findModel() {
    // The standard rig is a capsule root with the mesh on a child entity.
    if (auto* mc = entity().getComponent<ModelComponent>()) {
        mc->animate = false; // the animator owns sampling now
        modelEntity_ = &entity();
        return mc;
    }
    for (Entity* c : entity().children()) {
        if (auto* mc = c->getComponent<ModelComponent>()) {
            mc->animate = false;
            modelEntity_ = c;
            return mc;
        }
    }
    return nullptr;
}

void AnimatorComponent::enterState(int layer, int idx) {
    LayerRT& rt = layers_[layer];
    const AnimLayer& L = graph_.layers[layer];
    if (idx < 0 || idx >= (int)L.states.size()) return;
    rt.prev = rt.cur;
    rt.prevTime = rt.curTime;
    rt.cur = idx;
    rt.curTime = 0.0f;
    if (layer == 0) rootPrevValid_ = false; // don't turn the pose jump into motion
    if (model_ && L.states[idx].type == 0 && model_->clipIndex(L.states[idx].clip) < 0)
        AE_WARN("[Anim] clip '%s' not found in model (state '%s')",
                L.states[idx].clip.c_str(), L.states[idx].id.c_str());
}

void AnimatorComponent::onStart() {
    started_ = true;
    mc_ = findModel();
    model_ = mc_ ? mc_->model : nullptr;
    layers_.assign(graph_.layers.size(), LayerRT{});
    if (!model_) return;
    if (mc_->pose().empty()) model_->initPose(mc_->pose());
    model_->initPose(scratchState_);
    model_->initPose(scratchMotion_);
    model_->initPose(scratchLayer_);
    for (size_t li = 0; li < graph_.layers.size(); ++li) {
        const AnimLayer& L = graph_.layers[li];
        if (L.states.empty()) continue;
        int start = L.stateIndex(L.start);
        enterState((int)li, start < 0 ? 0 : start);
        layers_[li].prev = -1;
        layers_[li].blendT = layers_[li].blendDur = 0.0f;
    }
    // Root motion bone + its bind translation (the pose keeps the bind XZ).
    rootNode_ = rootBone.empty() ? model_->guessRootBone() : model_->nodeIndex(rootBone);
    if (rootMotion && rootNode_ < 0)
        AE_WARN("[Anim] root motion enabled but no root bone found (rootBone '%s')",
                rootBone.c_str());
    if (rootNode_ >= 0) rootBind_ = mc_->pose().locals[rootNode_].t;
    rootPrevValid_ = false;
    if (!graph_.layers.empty() && !graph_.layers[0].states.empty())
        AE_LOG("[Anim] start state '%s'", currentState().c_str());
}

// Blend-space weights at the state's current parameter point. 1D: inverse
// lerp between the two bracketing motions. 2D: gradient-band interpolation
// (Johansen) — robust for arbitrary motion layouts, no triangulation.
void AnimatorComponent::motionWeights(const AnimState& s, std::vector<float>& w) const {
    size_t n = s.motions.size();
    w.assign(n, 0.0f);
    if (n == 0) return;
    if (n == 1) {
        w[0] = 1.0f;
        return;
    }
    if (s.type == 1) {
        float p = param(s.paramX);
        // Bracketing motions on the x axis (order-independent).
        int lo = -1, hi = -1;
        for (size_t i = 0; i < n; ++i) {
            float x = s.motions[i].x;
            if (x <= p && (lo < 0 || x > s.motions[lo].x)) lo = (int)i;
            if (x >= p && (hi < 0 || x < s.motions[hi].x)) hi = (int)i;
        }
        if (lo < 0) { w[hi] = 1.0f; return; }
        if (hi < 0 || lo == hi) { w[lo] = 1.0f; return; }
        float span = s.motions[hi].x - s.motions[lo].x;
        float f = span > 1e-6f ? (p - s.motions[lo].x) / span : 0.0f;
        w[lo] = 1.0f - f;
        w[hi] = f;
        return;
    }
    // blend2d
    float px = param(s.paramX), py = param(s.paramY);
    float sum = 0.0f;
    for (size_t i = 0; i < n; ++i) {
        float wi = 1.0f;
        for (size_t j = 0; j < n && wi > 0.0f; ++j) {
            if (j == i) continue;
            float dx = s.motions[j].x - s.motions[i].x;
            float dy = s.motions[j].y - s.motions[i].y;
            float len2 = dx * dx + dy * dy;
            if (len2 < 1e-8f) continue;
            float proj = ((px - s.motions[i].x) * dx + (py - s.motions[i].y) * dy) / len2;
            wi = std::fmin(wi, clampf(1.0f - proj, 0.0f, 1.0f));
        }
        w[i] = wi;
        sum += wi;
    }
    if (sum > 1e-6f)
        for (float& wi : w) wi /= sum;
    else
        w[0] = 1.0f;
}

float AnimatorComponent::stateDuration(int li, int stateIdx) const {
    const AnimLayer& L = graph_.layers[li];
    if (stateIdx < 0 || stateIdx >= (int)L.states.size() || !model_) return 0.0f;
    const AnimState& s = L.states[stateIdx];
    if (s.type == 0) {
        int clip = model_->clipIndex(s.clip);
        return clip >= 0 && s.speed > 1e-4f ? model_->clipDuration(clip) / s.speed : 0.0f;
    }
    std::vector<float> w;
    motionWeights(s, w);
    float dur = 0.0f;
    for (size_t i = 0; i < s.motions.size(); ++i) {
        int clip = model_->clipIndex(s.motions[i].clip);
        float sp = s.motions[i].speed > 1e-4f ? s.motions[i].speed : 1.0f;
        if (clip >= 0) dur += w[i] * model_->clipDuration(clip) / sp;
    }
    return s.speed > 1e-4f ? dur / s.speed : 0.0f;
}

void AnimatorComponent::advanceTime(int li, float dt) {
    LayerRT& rt = layers_[li];
    const AnimLayer& L = graph_.layers[li];
    rt.wrapped = false;
    auto advance = [&](int stateIdx, float& time, bool isCur) {
        if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return;
        const AnimState& s = L.states[stateIdx];
        if (s.type == 0) {
            int clip = model_ ? model_->clipIndex(s.clip) : -1;
            float dur = clip >= 0 ? model_->clipDuration(clip) : 0.0f;
            float before = time;
            time += dt * s.speed;
            if (isCur && s.loop && dur > 1e-4f &&
                std::fmod(time, dur) < std::fmod(before, dur))
                rt.wrapped = true;
        } else {
            float dur = stateDuration(li, stateIdx); // seconds for one cycle
            time += dur > 1e-4f ? dt / dur : 0.0f;
            if (s.loop && time >= 1.0f) {
                time = std::fmod(time, 1.0f);
                if (isCur) rt.wrapped = true;
            } else if (!s.loop && time > 1.0f) {
                time = 1.0f;
            }
        }
    };
    advance(rt.cur, rt.curTime, true);
    advance(rt.prev, rt.prevTime, false);
    if (rt.blendDur > 0.0f) rt.blendT += dt;
}

void AnimatorComponent::evalState(int li, int stateIdx, float time, ModelPose& out) {
    model_->resetPoseLocals(out);
    const AnimLayer& L = graph_.layers[li];
    if (stateIdx < 0 || stateIdx >= (int)L.states.size()) return;
    const AnimState& s = L.states[stateIdx];

    if (s.type == 0) {
        int clip = model_->clipIndex(s.clip);
        if (clip < 0) return;
        float dur = model_->clipDuration(clip);
        float t = dur <= 0.0f ? 0.0f
                              : (s.loop ? std::fmod(time, dur) : std::fmin(time, dur));
        model_->evalClip(clip, t, out);
        return;
    }

    // Blend space: `time` is a normalized phase, so all motions stay in sync
    // (left/right feet line up when walking blends into running).
    std::vector<float> w;
    motionWeights(s, w);
    float phase = clampf(time, 0.0f, 1.0f);
    float wsum = 0.0f;
    bool first = true;
    for (size_t i = 0; i < s.motions.size(); ++i) {
        if (w[i] <= 0.001f) continue;
        int clip = model_->clipIndex(s.motions[i].clip);
        if (clip < 0) continue;
        float t = phase * model_->clipDuration(clip);
        if (first) {
            model_->evalClip(clip, t, out);
            wsum = w[i];
            first = false;
        } else {
            model_->resetPoseLocals(scratchMotion_);
            model_->evalClip(clip, t, scratchMotion_);
            wsum += w[i];
            Model::blendPose(out, scratchMotion_, w[i] / wsum);
        }
    }
}

void AnimatorComponent::applyRootMotion(float dt) {
    (void)dt;
    if (!rootMotion || rootNode_ < 0 || !modelEntity_) return;
    ModelPose& pose = mc_->pose();
    Vec3 cur = pose.locals[rootNode_].t;

    if (rootPrevValid_) {
        Vec3 delta = cur - rootPrevPos_;
        // Compensate the loop seam: add the clip's full root span.
        LayerRT& rt = layers_[0];
        if (rt.wrapped && rt.cur >= 0 && !graph_.layers.empty()) {
            const AnimState& s = graph_.layers[0].states[rt.cur];
            Vec3 span(0, 0, 0);
            auto clipSpan = [&](const std::string& clipName, float weight) {
                int clip = model_->clipIndex(clipName);
                if (clip < 0) return;
                Vec3 a, b;
                if (model_->clipTranslation(clip, rootNode_, 0.0f, a) &&
                    model_->clipTranslation(clip, rootNode_, model_->clipDuration(clip), b))
                    span = span + (b - a) * weight;
            };
            if (s.type == 0) {
                clipSpan(s.clip, 1.0f);
            } else {
                std::vector<float> w;
                motionWeights(s, w);
                for (size_t i = 0; i < s.motions.size(); ++i)
                    if (w[i] > 0.001f) clipSpan(s.motions[i].clip, w[i]);
            }
            delta = delta + span;
        }

        // Model space -> world direction via the model entity's world matrix.
        const Mat4& mw = modelEntity_->worldMatrix();
        Vec3 wd(mw.m[0][0] * delta.x + mw.m[1][0] * delta.y + mw.m[2][0] * delta.z,
                mw.m[0][1] * delta.x + mw.m[1][1] * delta.y + mw.m[2][1] * delta.z,
                mw.m[0][2] * delta.x + mw.m[1][2] * delta.y + mw.m[2][2] * delta.z);
        wd.y = 0.0f; // locomotion only; vertical stays authored in the pose

        // World -> the animator entity's parent space (root characters: 1:1).
        if (Entity* par = entity().parent()) {
            Mat4 inv = inverse(par->worldMatrix());
            wd = Vec3(inv.m[0][0] * wd.x + inv.m[1][0] * wd.y + inv.m[2][0] * wd.z,
                      inv.m[0][1] * wd.x + inv.m[1][1] * wd.y + inv.m[2][1] * wd.z,
                      inv.m[0][2] * wd.x + inv.m[1][2] * wd.y + inv.m[2][2] * wd.z);
        }
        entity().transform.position += wd;
    }

    rootPrevPos_ = cur;
    rootPrevValid_ = true;

    // Strip the horizontal root translation so the mesh doesn't double-move.
    pose.locals[rootNode_].t.x = rootBind_.x;
    pose.locals[rootNode_].t.z = rootBind_.z;
}

void AnimatorComponent::onUpdate(float dt) {
    if (!started_ || !model_ || !mc_) return;
    ModelPose& pose = mc_->pose();
    if (pose.empty()) model_->initPose(pose);

    // Advance every layer's state machine (authored order; first match wins).
    for (size_t li = 0; li < layers_.size() && li < graph_.layers.size(); ++li) {
        LayerRT& rt = layers_[li];
        const AnimLayer& L = graph_.layers[li];
        if (rt.cur < 0) continue;
        advanceTime((int)li, dt);
        const AnimState& s = L.states[rt.cur];
        for (const auto& t : L.transitions) {
            if (t.from != s.id) continue;
            if (!animConditionMet(t.op, param(t.param), t.value)) continue;
            int to = L.stateIndex(t.to);
            if (to < 0 || to == rt.cur) continue;
            enterState((int)li, to);
            rt.blendT = 0.0f;
            rt.blendDur = t.blend;
            AE_LOG("[Anim] %s%s: %s -> %s", li ? "layer " : "",
                   li ? L.name.c_str() : "", t.from.c_str(), t.to.c_str());
            break;
        }
    }

    // Base layer (with crossfade), then overlay layers masked/weighted on top.
    bool posed = false;
    for (size_t li = 0; li < layers_.size() && li < graph_.layers.size(); ++li) {
        LayerRT& rt = layers_[li];
        const AnimLayer& L = graph_.layers[li];
        if (rt.cur < 0) continue;

        ModelPose& target = posed ? scratchLayer_ : pose;
        bool fading = rt.blendDur > 0.0f && rt.blendT < rt.blendDur && rt.prev >= 0;
        if (fading) {
            evalState((int)li, rt.prev, rt.prevTime, target);
            evalState((int)li, rt.cur, rt.curTime, scratchState_);
            Model::blendPose(target, scratchState_, rt.blendT / rt.blendDur);
        } else {
            evalState((int)li, rt.cur, rt.curTime, target);
        }

        if (!posed) {
            posed = true;
            continue;
        }
        float lw = L.weightParam.empty() ? L.weight : param(L.weightParam);
        lw = clampf(lw, 0.0f, 1.0f);
        if (lw <= 0.0f) continue;
        if (!rt.maskBuilt) {
            rt.mask = L.maskBone.empty() ? std::vector<float>{}
                                         : model_->subtreeMask(L.maskBone);
            if (!L.maskBone.empty() && rt.mask.empty())
                AE_WARN("[Anim] layer '%s': mask bone '%s' not found in model",
                        L.name.c_str(), L.maskBone.c_str());
            rt.maskBuilt = true;
        }
        Model::blendPose(pose, target, lw, rt.mask.empty() ? nullptr : &rt.mask);
    }
    if (!posed) return;

    applyRootMotion(dt);
    model_->finalizePose(pose);
}

} // namespace ae
