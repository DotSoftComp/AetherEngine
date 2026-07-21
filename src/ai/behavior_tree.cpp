// Aether Engine — behavior tree + perception runtime (see behavior_tree.h).
#include "behavior_tree.h"
#include "nav_agent.h"
#include "../engine/world.h"
#include "../engine/assets.h"
#include "../engine/engine_modules.h"
#include "../physics/physics.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cmath>
#include <fstream>
#include <sstream>

namespace ae {

// ---- perception -----------------------------------------------------------
void PerceptionComponent::onUpdate(float) {
    target_ = nullptr;
    World& w = world();
    Vec3 eye = entity().worldPosition();
    Vec3 fwd = quatRotate(entity().transform.rotation, Vec3(0, 0, -1)); // local -Z
    float cosHalf = std::cos(radians(clampf(sightFovDeg, 0.0f, 360.0f) * 0.5f));

    float bestDist = 1e30f;
    for (const auto& up : w.entities()) {
        Entity* e = up.get();
        if (!e || e == &entity() || !e->active()) continue;
        if (targetTag.empty() || e->name().find(targetTag) == std::string::npos) continue;

        Vec3 to = e->worldPosition() - eye;
        float dist = length(to);
        if (dist < 1e-4f) continue;
        Vec3 dir = to * (1.0f / dist);

        bool sensed = false;
        // Hearing: omnidirectional, ignores facing + LOS.
        if (hearingRange > 0.0f && dist <= hearingRange) sensed = true;
        // Sight: within range and the view cone.
        if (!sensed && dist <= sightRange && dot(fwd, dir) >= cosHalf) {
            sensed = true;
            if (requireLineOfSight && engineModules().enabled("physics")) {
                RayHit hit = w.physics.raycast(w, eye, dir, dist - 0.05f);
                if (hit && hit.entity != e) sensed = false; // something blocks the view
            }
        }
        if (sensed && dist < bestDist) {
            bestDist = dist;
            target_ = e;
            lastKnownPos_ = e->worldPosition();
        }
    }
}

// ---- behavior tree (de)serialization --------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// Recursively flattens a nested JSON node into the flat node list; returns the
// new node's index.
static int parseNode(const JsonValue& jn, BehaviorTree& t) {
    int idx = (int)t.nodes.size();
    t.nodes.emplace_back();
    {
        BTNode n;
        if (const std::string* s = jn.string("type")) n.type = *s;
        if (const std::string* s = jn.string("s")) n.s = *s;
        else if (const std::string* m = jn.string("message")) n.s = *m;
        n.f = (float)jn.num("time", jn.num("f", jn.num("count", 0.0)));
        if (const JsonValue* a = jn.find("v"))
            if (a->size() == 3)
                n.v = Vec3((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number);
        t.nodes[idx] = std::move(n); // fields set before children recurse-append
    }
    if (const JsonValue* kids = jn.find("children"))
        for (size_t i = 0; i < kids->size(); ++i) {
            int child = parseNode((*kids)[i], t);
            t.nodes[idx].children.push_back(child);
        }
    // Decorators accept a single "child" object too.
    if (const JsonValue* kid = jn.find("child"))
        t.nodes[idx].children.push_back(parseNode(*kid, t));
    return idx;
}

bool loadBehaviorTree(BehaviorTree& t, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[BT] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("behaviorTree")) {
        AE_ERROR("[BT] malformed tree: %s", path.c_str());
        return false;
    }
    t = BehaviorTree{};
    const JsonValue* r = root.find("root");
    if (!r) {
        AE_ERROR("[BT] %s has no root node", path.c_str());
        return false;
    }
    parseNode(*r, t);
    return !t.nodes.empty();
}

static void writeNode(std::ostringstream& o, const BehaviorTree& t, int idx, int indent) {
    const BTNode& n = t.nodes[idx];
    std::string pad(indent, ' ');
    o << pad << "{ \"type\": \"" << esc(n.type) << "\"";
    if (!n.s.empty()) o << ", \"s\": \"" << esc(n.s) << "\"";
    if (n.f != 0.0f) o << ", \"time\": " << n.f;
    if (n.v.x != 0 || n.v.y != 0 || n.v.z != 0)
        o << ", \"v\": [" << n.v.x << ", " << n.v.y << ", " << n.v.z << "]";
    if (!n.children.empty()) {
        o << ", \"children\": [\n";
        for (size_t i = 0; i < n.children.size(); ++i) {
            writeNode(o, t, n.children[i], indent + 4);
            o << (i + 1 < n.children.size() ? "," : "") << "\n";
        }
        o << pad << "] }";
    } else {
        o << " }";
    }
}

bool saveBehaviorTree(const BehaviorTree& t, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"behaviorTree\": 1,\n  \"root\":\n";
    if (!t.nodes.empty()) writeNode(o, t, 0, 4);
    else o << "    {}";
    o << "\n}\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[BT] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[BT] saved %s (%d nodes)", path.c_str(), (int)t.nodes.size());
    return f.good();
}

// ---- BehaviorTreeComponent ------------------------------------------------
void BehaviorTreeComponent::onDeserialized(AssetLibrary& assets) {
    tree_ = BehaviorTree{};
    rt_.clear();
    bb_ = Blackboard{};
    activeLeaf_ = -1;
    if (treePath.empty()) return;
    std::string rel = treePath;
    if (!loadBehaviorTree(tree_, assets.resolvePath(rel))) return;
    treePath = rel;
    rt_.resize(tree_.nodes.size());
}

void BehaviorTreeComponent::onUpdate(float dt) {
    if (tree_.nodes.empty()) return;
    if (rt_.size() != tree_.nodes.size()) rt_.assign(tree_.nodes.size(), RT{});
    dt_ = dt;
    activeLeaf_ = -1;
    tick(0, dt);
}

// Recursive tick. `dt_`/`bb_` are members so leaves reach them without params.
BTStatus BehaviorTreeComponent::tick(int idx, float dt) {
    if (idx < 0 || idx >= (int)tree_.nodes.size()) return BTStatus::Failure;
    const BTNode& n = tree_.nodes[idx];
    RT& st = rt_[idx];
    const auto& kids = n.children;

    // ---- composites (stateless / reactive: re-evaluated from child 0) ----
    if (n.type == "Sequence") {
        for (int k : kids) {
            BTStatus s = tick(k, dt);
            if (s != BTStatus::Success) return s; // Running or Failure short-circuits
        }
        return BTStatus::Success;
    }
    if (n.type == "Selector") {
        for (int k : kids) {
            BTStatus s = tick(k, dt);
            if (s != BTStatus::Failure) return s; // Running or Success short-circuits
        }
        return BTStatus::Failure;
    }
    if (n.type == "Parallel") {
        // Succeeds when all children have succeeded; fails if any fails.
        bool anyRunning = false;
        for (int k : kids) {
            BTStatus s = tick(k, dt);
            if (s == BTStatus::Failure) return BTStatus::Failure;
            if (s == BTStatus::Running) anyRunning = true;
        }
        return anyRunning ? BTStatus::Running : BTStatus::Success;
    }

    // ---- decorators ----
    if (n.type == "Inverter") {
        if (kids.empty()) return BTStatus::Failure;
        BTStatus s = tick(kids[0], dt);
        if (s == BTStatus::Success) return BTStatus::Failure;
        if (s == BTStatus::Failure) return BTStatus::Success;
        return BTStatus::Running;
    }
    if (n.type == "Repeat") {
        if (kids.empty()) return BTStatus::Failure;
        tick(kids[0], dt);
        // The child restarts each cycle; the loop itself never "finishes"
        // (always Running so a parent Selector can still pre-empt it).
        return BTStatus::Running;
    }
    if (n.type == "Succeeder") {
        if (!kids.empty()) tick(kids[0], dt);
        return BTStatus::Success;
    }

    // ---- leaves ----
    activeLeaf_ = idx;
    if (n.type == "Wait") {
        st.timer += dt;
        if (st.timer >= n.f) { st.timer = 0.0f; return BTStatus::Success; }
        return BTStatus::Running;
    }
    if (n.type == "Log") {
        AE_LOG("[BT] %s: %s", entity().name().c_str(), n.s.c_str());
        return BTStatus::Success;
    }
    if (n.type == "CanSeeTarget") {
        auto* p = entity().getComponent<PerceptionComponent>();
        if (p && p->hasTarget()) {
            bb_.target = p->target();
            bb_.targetPos = p->lastKnownPos();
            bb_.hasTarget = true;
            return BTStatus::Success;
        }
        return BTStatus::Failure;
    }
    if (n.type == "HasTarget") {
        return bb_.hasTarget ? BTStatus::Success : BTStatus::Failure;
    }
    if (n.type == "ClearTarget") {
        bb_ = Blackboard{};
        return BTStatus::Success;
    }
    if (n.type == "MoveToTarget" || n.type == "MoveToPoint") {
        auto* agent = entity().getComponent<NavAgentComponent>();
        if (!agent) return BTStatus::Failure;
        if (n.type == "MoveToTarget" && !bb_.hasTarget) return BTStatus::Failure;
        Vec3 goal = n.type == "MoveToPoint" ? n.v : bb_.targetPos;
        // (Re)issue when starting fresh or the goal moved — chase tracks a
        // moving target since the composites re-tick CanSeeTarget each frame.
        if (!st.issued || length(goal - st.lastGoal) > 0.5f) {
            agent->moveTo(goal);
            st.issued = true;
            st.lastGoal = goal;
            return BTStatus::Running;
        }
        if (agent->arrived()) { st.issued = false; return BTStatus::Success; }
        if (!agent->isMoving()) { st.issued = false; return BTStatus::Failure; } // gave up
        return BTStatus::Running;
    }
    if (n.type == "IsAtTarget") {
        auto* agent = entity().getComponent<NavAgentComponent>();
        return agent && agent->arrived() ? BTStatus::Success : BTStatus::Failure;
    }
    if (n.type == "Stop") {
        if (auto* agent = entity().getComponent<NavAgentComponent>()) agent->stop();
        return BTStatus::Success;
    }

    AE_WARN("[BT] %s: unknown node type '%s'", entity().name().c_str(), n.type.c_str());
    return BTStatus::Failure;
}

} // namespace ae
