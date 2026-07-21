// Aether Engine — visual-scripting v2 runtime (see script_graph.h).
//
// ADDING A NODE: append one def() block to buildDefs() below — category, pins,
// param labels, and the eval lambda. The serializer, runtime, and canvas editor
// pick it up automatically.
#include "script_graph.h"
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/assets.h"
#include "../engine/components.h"
#include "../engine/scene_io.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../audio/audio.h"
// Leaf includes (mirror component_registry.cpp): bridges into other modules
// stay out of engine headers.
#include "../narrative/dialogue_trigger.h"
#include "../physics/physics_components.h"
#include "../engine/anim_graph.h"
#include "../ai/nav_agent.h"
#include "../render/debug_draw.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <random>
#include <sstream>

namespace ae {

// ---- typed values -------------------------------------------------------------
const char* pinTypeName(PinType t) {
    switch (t) {
    case PinType::Exec: return "Exec";
    case PinType::Float: return "Float";
    case PinType::Vec3: return "Vec3";
    case PinType::Bool: return "Bool";
    case PinType::String: return "String";
    case PinType::Entity: return "Entity";
    }
    return "?";
}

unsigned pinTypeColor(PinType t) { // ABGR (IM_COL32-compatible)
    switch (t) {
    case PinType::Exec: return 0xffffffffu;   // white
    case PinType::Float: return 0xff6fd66fu;  // green
    case PinType::Vec3: return 0xff46b4f0u;   // orange
    case PinType::Bool: return 0xff5a5ae6u;   // red
    case PinType::String: return 0xffd264d2u; // magenta
    case PinType::Entity: return 0xffd2c850u; // cyan
    }
    return 0xffffffffu;
}

float Value::asF() const {
    switch (type) {
    case PinType::Float: return f;
    case PinType::Vec3: return v.x;
    case PinType::Bool: return b ? 1.0f : 0.0f;
    case PinType::String: return (float)std::atof(s.c_str());
    default: return 0.0f;
    }
}
Vec3 Value::asV() const {
    switch (type) {
    case PinType::Vec3: return v;
    case PinType::Float: return Vec3(f);
    case PinType::Bool: return Vec3(b ? 1.0f : 0.0f);
    default: return Vec3(0);
    }
}
bool Value::asB() const {
    switch (type) {
    case PinType::Bool: return b;
    case PinType::Float: return f != 0.0f;
    case PinType::Vec3: return dot(v, v) > 0.0f;
    case PinType::String: return !s.empty();
    case PinType::Entity: return ent != 0;
    default: return false;
    }
}
std::string Value::asS(const World* w) const {
    char buf[64];
    switch (type) {
    case PinType::String: return s;
    case PinType::Float: std::snprintf(buf, sizeof(buf), "%g", f); return buf;
    case PinType::Vec3:
        std::snprintf(buf, sizeof(buf), "(%.3g, %.3g, %.3g)", v.x, v.y, v.z);
        return buf;
    case PinType::Bool: return b ? "true" : "false";
    case PinType::Entity: {
        if (w) if (Entity* e = w->findById(ent)) return e->name();
        std::snprintf(buf, sizeof(buf), "entity#%u", ent);
        return buf;
    }
    default: return "";
    }
}

static PinType pinTypeFromName(const std::string& n) {
    if (n == "Vec3") return PinType::Vec3;
    if (n == "Bool") return PinType::Bool;
    if (n == "String") return PinType::String;
    if (n == "Entity") return PinType::Entity;
    return PinType::Float;
}

// ---- node registry --------------------------------------------------------------
namespace {

// Tiny builder so each node reads as a single declarative block.
struct Def {
    NodeDef d;
    Def(const char* type, const char* category) {
        d.type = type;
        d.category = category;
    }
    Def& event() { d.isEvent = true; return *this; }
    Def& exec() { d.execIn = {"in"}; d.execOut = {"then"}; return *this; }
    Def& execOuts(std::initializer_list<const char*> outs) {
        d.execIn = d.isEvent ? std::vector<std::string>{} : std::vector<std::string>{"in"};
        d.execOut.clear();
        for (auto* o : outs) d.execOut.push_back(o);
        return *this;
    }
    Def& eventOuts(std::initializer_list<const char*> outs) { // event: no exec-in
        d.execIn.clear();
        d.execOut.clear();
        for (auto* o : outs) d.execOut.push_back(o);
        return *this;
    }
    Def& in(const char* name, PinType t, Value def = {}) {
        // An unconnected pin must default to its own type's zero. Value{} is a
        // Float 0, and a Float 0 read as a string is "0" — which silently broke
        // every "leave the input empty to use the param instead" node, because
        // the input was never actually empty.
        if (def.type == PinType::Float && def.f == 0.0f && t != PinType::Float) {
            switch (t) {
            case PinType::Vec3: def = Value::V(Vec3(0)); break;
            case PinType::Bool: def = Value::B(false); break;
            case PinType::String: def = Value::S(""); break;
            case PinType::Entity: def = Value::E(0); break;
            default: break;
            }
        }
        d.dataIn.push_back({name, t, def});
        return *this;
    }
    Def& out(const char* name, PinType t) {
        d.dataOut.push_back({name, t, {}});
        return *this;
    }
    Def& params(const char* pLabel, const char* nLabel = nullptr) {
        d.pLabel = pLabel;
        d.nLabel = nLabel;
        return *this;
    }
    Def& run(std::function<int(NodeCtx&)> fn) { d.eval = std::move(fn); return *this; }
};

std::vector<NodeDef> buildDefs() {
    std::vector<Def> b;
    auto quat = [](Entity* e) { return e ? e->transform.rotation : quatIdentity(); };
    (void)quat;

    // ================= Events =================
    b.emplace_back("OnStart", "Events");
    b.back().event().eventOuts({"then"}).run([](NodeCtx&) { return 0; });

    b.emplace_back("OnUpdate", "Events");
    b.back().event().eventOuts({"then"}).out("Dt", PinType::Float).run([](NodeCtx&) { return 0; });

    b.emplace_back("OnKey", "Events");
    b.back().event().eventOuts({"pressed"}).params("Key (single char)").run(
        [](NodeCtx&) { return 0; });

    b.emplace_back("OnPlayerNear", "Events");
    b.back().event().eventOuts({"then"}).out("Player", PinType::Entity).params(
        "Player entity (def MainCamera)", "Radius").run([](NodeCtx&) { return 0; });

    b.emplace_back("OnFlag", "Events");
    b.back().event().eventOuts({"then"}).params("Flag", "Equals value").run(
        [](NodeCtx&) { return 0; });

    b.emplace_back("OnUIButton", "Events");
    b.back().event().eventOuts({"clicked"}).params("Button id").run([](NodeCtx&) { return 0; });

    b.emplace_back("OnAction", "Events");
    b.back().event().eventOuts({"pressed"}).params("Action name (input map)").run(
        [](NodeCtx&) { return 0; });

    // Receives a message aimed at THIS entity by another entity's SendEvent
    // (or by BroadcastEvent). Fires once per message, in the frame it arrives.
    b.emplace_back("OnEvent", "Events");
    b.back().event().eventOuts({"then"}).out("Value", PinType::Float)
        .out("Sender", PinType::Entity).params("Event name").run([](NodeCtx&) { return 0; });

    // ================= Flow =================
    b.emplace_back("If", "Flow");
    b.back().execOuts({"true", "false"}).in("Cond", PinType::Bool).run(
        [](NodeCtx& c) { return c.inB(0) ? 0 : 1; });

    b.emplace_back("Sequence", "Flow");
    // Fires 1 then 2 then 3? A token is linear — Sequence here means "first
    // connected pin" chains run one after another is NOT possible without
    // sub-tokens; instead we SPAWN a token per pin. Simple + parallel.
    b.back().execOuts({"a", "b", "c"}).run([](NodeCtx&) { return 0; }); // special-cased in step()

    b.emplace_back("Delay", "Flow");
    b.back().exec().in("Seconds", PinType::Float, Value::F(1.0f)).run([](NodeCtx& c) {
        c.park(c.inF(0));
        return kParked;
    });

    b.emplace_back("Once", "Flow");
    b.back().exec().run([](NodeCtx&) { return 0; }); // gate handled in step()

    b.emplace_back("ForLoop", "Flow");
    // Wire the END of the body chain back into this node to iterate: each entry
    // increments the counter until Count is reached, then leaves via "done".
    b.back().execOuts({"body", "done"}).in("Count", PinType::Float, Value::F(3)).out(
        "Index", PinType::Float).run([](NodeCtx&) { return 0; }); // special-cased

    // ================= Functions =================
    b.emplace_back("Function", "Functions");
    b.back().eventOuts({"body"}).params("Function name").run([](NodeCtx&) { return 0; });
    b.back().d.isEvent = false; // entered only via Call, never auto-fired

    b.emplace_back("Call", "Functions");
    b.back().exec().params("Function name").run([](NodeCtx&) { return 0; }); // special-cased

    b.emplace_back("Return", "Functions");
    b.back().d.execIn = {"in"};
    b.back().run([](NodeCtx&) { return kEnd; }); // pop handled in step()

    // ================= Variables =================
    b.emplace_back("GetVar", "Variables");
    b.back().out("Value", PinType::Float).params("Variable name").run([](NodeCtx& c) {
        c.out(0, c.comp.getVar(c.node.p));
        return kEnd;
    });

    b.emplace_back("SetVar", "Variables");
    b.back().exec().in("Value", PinType::Float).params("Variable name").run([](NodeCtx& c) {
        c.comp.setVar(c.node.p, c.in(0));
        return 0;
    });

    // ================= Entity =================
    b.emplace_back("Self", "Entity");
    b.back().out("Entity", PinType::Entity).run([](NodeCtx& c) {
        c.out(0, Value::E(c.self().id()));
        return kEnd;
    });

    b.emplace_back("FindEntity", "Entity");
    b.back().out("Entity", PinType::Entity).in("Name", PinType::String).params(
        "Name (if input empty)").run([](NodeCtx& c) {
        std::string n = c.inS(0);
        if (n.empty()) n = c.node.p;
        Entity* e = c.world.find(n);
        c.out(0, Value::E(e ? e->id() : 0));
        return kEnd;
    });

    b.emplace_back("GetPosition", "Entity");
    b.back().in("Entity", PinType::Entity).out("Position", PinType::Vec3).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        c.out(0, Value::V(e ? e->transform.position : Vec3(0)));
        return kEnd;
    });

    b.emplace_back("SetPosition", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Position", PinType::Vec3).run(
        [](NodeCtx& c) {
            if (Entity* e = c.inE(0)) e->transform.position = c.inV(1);
            return 0;
        });

    b.emplace_back("Translate", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Delta", PinType::Vec3).run([](NodeCtx& c) {
        if (Entity* e = c.inE(0)) e->transform.position += c.inV(1);
        return 0;
    });

    b.emplace_back("RotateYaw", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Degrees", PinType::Float).run(
        [](NodeCtx& c) {
            if (Entity* e = c.inE(0)) e->transform.rotateAxis(Vec3(0, 1, 0), radians(c.inF(1)));
            return 0;
        });

    b.emplace_back("GetForward", "Entity");
    b.back().in("Entity", PinType::Entity).out("Forward", PinType::Vec3).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        c.out(0, Value::V(e ? quatRotate(e->transform.rotation, Vec3(0, 0, -1)) : Vec3(0, 0, -1)));
        return kEnd;
    });

    b.emplace_back("SetActive", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Active", PinType::Bool, Value::B(true)).run(
        [](NodeCtx& c) {
            if (Entity* e = c.inE(0)) e->setActive(c.inB(1));
            return 0;
        });

    b.emplace_back("DestroyEntity", "Entity");
    b.back().exec().in("Entity", PinType::Entity).run([](NodeCtx& c) {
        if (Entity* e = c.inE(0)) c.world.destroy(e);
        return 0;
    });

    // Instances a prefab asset at a position. The Spawned output lets the
    // caller immediately configure the new entity (SendEvent it a direction,
    // SetVarOn its health, ...) — the spawn-and-wire idiom every shooter needs.
    b.emplace_back("SpawnPrefab", "Entity");
    b.back().execOuts({"then"}).in("Position", PinType::Vec3).in("Yaw", PinType::Float)
        .out("Spawned", PinType::Entity).params("Prefab (assets/entities/*.json)")
        .run([](NodeCtx& c) {
            AssetLibrary* assets = c.comp.assets();
            if (!assets || c.node.p.empty()) return 0;
            Entity* e = instantiatePrefab(c.world, *assets, c.node.p);
            if (!e) return 0;
            e->transform.position = c.inV(0);
            float yaw = c.inF(1);
            if (yaw != 0.0f) e->transform.rotation = quatAxisAngle(Vec3(0, 1, 0), radians(yaw));
            c.out(0, Value::E(e->id()));
            return 0;
        });

    // ---- entity messaging ----
    b.emplace_back("SendEvent", "Entity");
    b.back().exec().in("Target", PinType::Entity).in("Value", PinType::Float)
        .params("Event name").run([](NodeCtx& c) {
            Entity* t = c.inE(0);
            auto* sg = t ? t->getComponent<ScriptGraphComponent>() : nullptr;
            if (sg) sg->postEvent(c.node.p, c.inF(1), c.self().id());
            return 0;
        });

    // Same message to every scripted entity — level-wide beats ("player died",
    // "alarm"), so a graph never has to hold a list of listeners.
    b.emplace_back("BroadcastEvent", "Entity");
    b.back().exec().in("Value", PinType::Float).params("Event name").run([](NodeCtx& c) {
        uint32_t from = c.self().id();
        for (const auto& e : c.world.entities())
            if (auto* sg = e->getComponent<ScriptGraphComponent>()) sg->postEvent(c.node.p, c.inF(0), from);
        return 0;
    });

    // ---- cross-entity variables ----
    b.emplace_back("GetVarOn", "Entity");
    b.back().in("Entity", PinType::Entity).out("Value", PinType::Float).params("Variable name")
        .run([](NodeCtx& c) {
            Entity* e = c.inE(0);
            auto* sg = e ? e->getComponent<ScriptGraphComponent>() : nullptr;
            c.out(0, sg ? sg->getVar(c.node.p) : Value{});
            return kEnd;
        });

    b.emplace_back("SetVarOn", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Value", PinType::Float)
        .params("Variable name").run([](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* sg = e ? e->getComponent<ScriptGraphComponent>() : nullptr)
                sg->setVar(c.node.p, c.in(1));
            return 0;
        });

    // Nearest entity whose name contains the filter — the cheap "who is near
    // me" query (enemies looking for the player, a shot looking for a victim)
    // that avoids hand-maintained entity lists.
    // Nearest entity whose name contains the filter — the cheap "who is near
    // me" query (enemies looking for the player, a shot looking for a victim)
    // that avoids hand-maintained entity lists.
    //
    // Direction + MinDot narrow it to a CONE, which is what targeting actually
    // needs: the nearest enemy overall is frequently the one standing behind
    // you, and an auto-aim that snaps backwards is worse than none. Leave
    // MinDot at -1 for "any direction".
    b.emplace_back("FindNearest", "Entity");
    b.back().in("Origin", PinType::Vec3).in("MaxDist", PinType::Float, Value::F(1e9f))
        .in("Direction", PinType::Vec3).in("MinDot", PinType::Float, Value::F(-1.0f))
        .out("Entity", PinType::Entity).out("Distance", PinType::Float)
        .out("Found", PinType::Bool)
        .params("Name contains").run([](NodeCtx& c) {
            Vec3 origin = c.inV(0);
            float maxD = c.inF(1), best = maxD * maxD;
            Vec3 dir = c.inV(2);
            float minDot = c.inF(3);
            bool coned = minDot > -1.0f && dot(dir, dir) > 1e-6f;
            if (coned) dir = normalize(dir);
            Entity* found = nullptr;
            for (const auto& e : c.world.entities()) {
                if (!e->active() || e.get() == &c.self()) continue;
                if (!c.node.p.empty() && e->name().find(c.node.p) == std::string::npos) continue;
                Vec3 d = e->worldPosition() - origin;
                float d2 = dot(d, d);
                if (d2 > best || d2 < 1e-8f) continue;
                if (coned && dot(d / std::sqrt(d2), dir) < minDot) continue;
                best = d2;
                found = e.get();
            }
            c.out(0, Value::E(found ? found->id() : 0));
            c.out(1, Value::F(found ? std::sqrt(best) : maxD));
            c.out(2, Value::B(found != nullptr));
            return kEnd;
        });

    // Yaw-only facing (characters stay upright), the form gameplay actually
    // wants — a full look-at would tip a walking enemy onto its nose.
    b.emplace_back("LookAt", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Target", PinType::Vec3).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        if (!e) return 0;
        Vec3 d = c.inV(1) - e->worldPosition();
        if (d.x * d.x + d.z * d.z < 1e-8f) return 0;
        e->transform.rotation = quatAxisAngle(Vec3(0, 1, 0), std::atan2(d.x, d.z) + PI);
        return 0;
    });

    // ---- live material tweaks (hit flashes, powered-down lights, ...) ----
    // Turns a collider into a sensor (and back). The moving-platform escape
    // hatch: a rising door or a lift that still has a solid body while it
    // travels will sweep whatever is standing in it, and the solver resolves
    // that penetration by launching the victim. Gameplay wants "stop colliding
    // for the duration of the move", which is exactly this.
    b.emplace_back("SetTrigger", "Physics");
    b.back().exec().in("Entity", PinType::Entity).in("IsTrigger", PinType::Bool, Value::B(true))
        .run([](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* col = e ? e->getComponent<ColliderComponent>() : nullptr)
                col->isTrigger = c.inB(1);
            return 0;
        });

    b.emplace_back("SetEmissive", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Color", PinType::Vec3).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        if (auto* mr = e ? e->getComponent<MeshRenderer>() : nullptr) mr->material.emissive = c.inV(1);
        return 0;
    });

    b.emplace_back("SetBaseColor", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Color", PinType::Vec3)
        .in("Alpha", PinType::Float, Value::F(1)).run([](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* mr = e ? e->getComponent<MeshRenderer>() : nullptr) {
                Vec3 rgb = c.inV(1);
                mr->material.baseColor = Vec4(rgb.x, rgb.y, rgb.z, c.inF(2));
            }
            return 0;
        });

    b.emplace_back("SetLight", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Intensity", PinType::Float)
        .in("Color", PinType::Vec3, Value::V(Vec3(-1, -1, -1))).run([](NodeCtx& c) {
            Entity* e = c.inE(0);
            auto* l = e ? e->getComponent<LightComponent>() : nullptr;
            if (!l) return 0;
            l->intensity = c.inF(1);
            Vec3 col = c.inV(2);
            if (col.x >= 0.0f) l->color = col; // negative = keep current color
            return 0;
        });

    // ================= Physics =================
    b.emplace_back("GetVelocity", "Physics");
    b.back().in("Entity", PinType::Entity).out("Velocity", PinType::Vec3).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        auto* rb = e ? e->getComponent<RigidBodyComponent>() : nullptr;
        c.out(0, Value::V(rb ? rb->velocity : Vec3(0)));
        return kEnd;
    });

    b.emplace_back("SetVelocity", "Physics");
    b.back().exec().in("Entity", PinType::Entity).in("Velocity", PinType::Vec3).run(
        [](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* rb = e ? e->getComponent<RigidBodyComponent>() : nullptr)
                rb->velocity = c.inV(1);
            return 0;
        });

    b.emplace_back("AddImpulse", "Physics");
    b.back().exec().in("Entity", PinType::Entity).in("Impulse", PinType::Vec3).run(
        [](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* rb = e ? e->getComponent<RigidBodyComponent>() : nullptr)
                rb->velocity += c.inV(1);
            return 0;
        });

    b.emplace_back("IsGrounded", "Physics");
    b.back().in("Entity", PinType::Entity).out("Grounded", PinType::Bool).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        auto* rb = e ? e->getComponent<RigidBodyComponent>() : nullptr;
        c.out(0, Value::B(rb ? rb->grounded : false));
        return kEnd;
    });

    b.emplace_back("IsOverlapping", "Physics");
    b.back().in("Entity", PinType::Entity).out("Overlapping", PinType::Bool).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        auto* col = e ? e->getComponent<ColliderComponent>() : nullptr;
        c.out(0, Value::B(col ? col->overlapping : false));
        return kEnd;
    });

    b.emplace_back("Raycast", "Physics");
    // Ignore takes the entity the ray starts inside — a shooter, or a monster
    // testing line of sight from its own head. Leave it unconnected and the
    // ray can hit whatever cast it.
    b.back().in("Origin", PinType::Vec3).in("Direction", PinType::Vec3, Value::V(Vec3(0, -1, 0)))
        .in("MaxDist", PinType::Float, Value::F(100)).in("Ignore", PinType::Entity)
        .out("Hit", PinType::Bool).out("Entity", PinType::Entity).out("Point", PinType::Vec3)
        .run([](NodeCtx& c) {
            RayHit h = c.world.physics.raycast(c.world, c.inV(0), c.inV(1), c.inF(2), c.inE(3));
            c.out(0, Value::B(h.hit));
            c.out(1, Value::E(h.entity ? h.entity->id() : 0));
            c.out(2, Value::V(h.point));
            return kEnd;
        });

    // ================= Math =================
    auto binF = [&](const char* name, float (*fn)(float, float), float bDef) {
        b.emplace_back(name, "Math");
        b.back().in("A", PinType::Float).in("B", PinType::Float, Value::F(bDef)).out(
            "Out", PinType::Float);
        auto f = fn;
        b.back().run([f](NodeCtx& c) { c.out(0, Value::F(f(c.inF(0), c.inF(1)))); return kEnd; });
    };
    binF("Add", [](float a, float x) { return a + x; }, 0.0f);
    binF("Subtract", [](float a, float x) { return a - x; }, 0.0f);
    binF("Multiply", [](float a, float x) { return a * x; }, 1.0f);
    binF("Divide", [](float a, float x) { return x != 0.0f ? a / x : 0.0f; }, 1.0f);
    binF("Min", [](float a, float x) { return a < x ? a : x; }, 0.0f);
    binF("Max", [](float a, float x) { return a > x ? a : x; }, 0.0f);
    binF("Power", [](float a, float x) { return std::pow(a, x); }, 2.0f);

    b.emplace_back("Sin", "Math");
    b.back().in("A", PinType::Float).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(std::sin(c.inF(0))));
        return kEnd;
    });
    b.emplace_back("Cos", "Math");
    b.back().in("A", PinType::Float).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(std::cos(c.inF(0))));
        return kEnd;
    });
    b.emplace_back("Abs", "Math");
    b.back().in("A", PinType::Float).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(std::fabs(c.inF(0))));
        return kEnd;
    });
    b.emplace_back("Clamp", "Math");
    b.back().in("A", PinType::Float).in("Min", PinType::Float, Value::F(0)).in(
        "Max", PinType::Float, Value::F(1)).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(clampf(c.inF(0), c.inF(1), c.inF(2))));
        return kEnd;
    });
    b.emplace_back("Lerp", "Math");
    b.back().in("A", PinType::Float).in("B", PinType::Float, Value::F(1)).in(
        "T", PinType::Float).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(lerpf(c.inF(0), c.inF(1), c.inF(2))));
        return kEnd;
    });
    b.emplace_back("Random", "Math");
    b.back().in("Min", PinType::Float, Value::F(0)).in("Max", PinType::Float, Value::F(1)).out(
        "Out", PinType::Float).run([](NodeCtx& c) {
        static std::mt19937 rng{std::random_device{}()};
        std::uniform_real_distribution<float> d(c.inF(0), c.inF(1));
        c.out(0, Value::F(d(rng)));
        return kEnd;
    });
    b.emplace_back("Time", "Math");
    b.back().out("Seconds", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(c.world.time()));
        return kEnd;
    });
    b.emplace_back("DeltaTime", "Math");
    b.back().out("Dt", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(c.world.dt()));
        return kEnd;
    });

    // ---- Vec3 ----
    b.emplace_back("MakeVec3", "Math");
    b.back().in("X", PinType::Float).in("Y", PinType::Float).in("Z", PinType::Float).out(
        "Vec", PinType::Vec3).run([](NodeCtx& c) {
        c.out(0, Value::V(Vec3(c.inF(0), c.inF(1), c.inF(2))));
        return kEnd;
    });
    b.emplace_back("BreakVec3", "Math");
    b.back().in("Vec", PinType::Vec3).out("X", PinType::Float).out("Y", PinType::Float).out(
        "Z", PinType::Float).run([](NodeCtx& c) {
        Vec3 v = c.inV(0);
        c.out(0, Value::F(v.x));
        c.out(1, Value::F(v.y));
        c.out(2, Value::F(v.z));
        return kEnd;
    });
    b.emplace_back("AddVec3", "Math");
    b.back().in("A", PinType::Vec3).in("B", PinType::Vec3).out("Out", PinType::Vec3).run(
        [](NodeCtx& c) {
            c.out(0, Value::V(c.inV(0) + c.inV(1)));
            return kEnd;
        });
    b.emplace_back("ScaleVec3", "Math");
    b.back().in("Vec", PinType::Vec3).in("Scale", PinType::Float, Value::F(1)).out(
        "Out", PinType::Vec3).run([](NodeCtx& c) {
        c.out(0, Value::V(c.inV(0) * c.inF(1)));
        return kEnd;
    });
    b.emplace_back("Dot", "Math");
    b.back().in("A", PinType::Vec3).in("B", PinType::Vec3).out("Out", PinType::Float)
        .run([](NodeCtx& c) {
            c.out(0, Value::F(dot(c.inV(0), c.inV(1))));
            return kEnd;
        });

    b.emplace_back("Distance", "Math");
    b.back().in("A", PinType::Vec3).in("B", PinType::Vec3).out("Out", PinType::Float).run(
        [](NodeCtx& c) {
            c.out(0, Value::F(length(c.inV(0) - c.inV(1))));
            return kEnd;
        });
    b.emplace_back("Normalize", "Math");
    b.back().in("Vec", PinType::Vec3).out("Out", PinType::Vec3).run([](NodeCtx& c) {
        c.out(0, Value::V(normalize(c.inV(0))));
        return kEnd;
    });
    b.emplace_back("Length", "Math");
    b.back().in("Vec", PinType::Vec3).out("Out", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F(length(c.inV(0))));
        return kEnd;
    });

    // ================= Logic =================
    b.emplace_back("Compare", "Logic");
    b.back().in("A", PinType::Float).in("B", PinType::Float).out("Out", PinType::Bool).params(
        "Op: > < >= <= == !=").run([](NodeCtx& c) {
        float a = c.inF(0), x = c.inF(1);
        const std::string& op = c.node.p;
        bool r = op == "<"    ? a < x
                 : op == ">=" ? a >= x
                 : op == "<=" ? a <= x
                 : op == "==" ? a == x
                 : op == "!=" ? a != x
                              : a > x; // default ">"
        c.out(0, Value::B(r));
        return kEnd;
    });
    b.emplace_back("And", "Logic");
    b.back().in("A", PinType::Bool).in("B", PinType::Bool).out("Out", PinType::Bool).run(
        [](NodeCtx& c) { c.out(0, Value::B(c.inB(0) && c.inB(1))); return kEnd; });
    b.emplace_back("Or", "Logic");
    b.back().in("A", PinType::Bool).in("B", PinType::Bool).out("Out", PinType::Bool).run(
        [](NodeCtx& c) { c.out(0, Value::B(c.inB(0) || c.inB(1))); return kEnd; });
    b.emplace_back("Not", "Logic");
    b.back().in("A", PinType::Bool).out("Out", PinType::Bool).run([](NodeCtx& c) {
        c.out(0, Value::B(!c.inB(0)));
        return kEnd;
    });
    b.emplace_back("IsActionDown", "Logic");
    b.back().out("Down", PinType::Bool).params("Action name").run([](NodeCtx& c) {
        c.out(0, Value::B(c.world.actions.down(c.node.p)));
        return kEnd;
    });
    b.emplace_back("GetAxis", "Logic");
    b.back().out("Value", PinType::Float).params("Axis name (e.g. MoveX)").run([](NodeCtx& c) {
        c.out(0, Value::F(c.world.actions.axis(c.node.p)));
        return kEnd;
    });

    b.emplace_back("IsKeyDown", "Logic");
    b.back().out("Down", PinType::Bool).params("Key (single char)").run([](NodeCtx& c) {
        bool down = false;
        if (!c.node.p.empty())
            down = c.world.input().keys[(unsigned char)std::toupper((unsigned char)c.node.p[0])];
        c.out(0, Value::B(down));
        return kEnd;
    });

    // ================= Values =================
    b.emplace_back("Float", "Values");
    b.back().out("Value", PinType::Float).params(nullptr, "Value").run([](NodeCtx& c) {
        c.out(0, Value::F(c.node.n));
        return kEnd;
    });
    b.emplace_back("String", "Values");
    b.back().out("Value", PinType::String).params("Text").run([](NodeCtx& c) {
        c.out(0, Value::S(c.node.p));
        return kEnd;
    });
    b.emplace_back("Append", "Values");
    b.back().in("A", PinType::String).in("B", PinType::String).out("Out", PinType::String).run(
        [](NodeCtx& c) { c.out(0, Value::S(c.inS(0) + c.inS(1))); return kEnd; });

    // ================= Game =================
    b.emplace_back("GetFlag", "Game");
    b.back().out("Value", PinType::Float).params("Flag").run([](NodeCtx& c) {
        c.out(0, Value::F((float)c.world.missions.flag(c.node.p)));
        return kEnd;
    });
    b.emplace_back("SetFlag", "Game");
    b.back().exec().in("Value", PinType::Float, Value::F(1)).params("Flag").run([](NodeCtx& c) {
        c.world.missions.setFlag(c.node.p, (int)c.inF(0));
        return 0;
    });
    // Read/modify a flag whose NAME is itself data. Without these, anything
    // table-driven (a weapon row that says which ammo pool it spends, a pickup
    // that says which counter it fills) has to fan out into one hard-coded
    // branch per flag; with them it stays one node.
    b.emplace_back("GetFlagNamed", "Game");
    b.back().in("Name", PinType::String).out("Value", PinType::Float).run([](NodeCtx& c) {
        c.out(0, Value::F((float)c.world.missions.flag(c.inS(0))));
        return kEnd;
    });
    b.emplace_back("SetFlagNamed", "Game");
    b.back().exec().in("Name", PinType::String).in("Value", PinType::Float).run([](NodeCtx& c) {
        c.world.missions.setFlag(c.inS(0), (int)c.inF(1));
        return 0;
    });

    // Accumulate with saturation — the shape almost every counter in a game
    // actually wants (health that caps, ammo that caps, a score that only
    // rises). Out is the value after clamping, so the caller can react to it.
    auto addFlag = [](NodeCtx& c, const std::string& name, float delta, float lo, float hi) {
        float v = (float)c.world.missions.flag(name) + delta;
        v = v < lo ? lo : (v > hi ? hi : v);
        c.world.missions.setFlag(name, (int)v);
        c.out(0, Value::F(v));
        return 0;
    };
    b.emplace_back("AddFlag", "Game");
    b.back().exec().in("Delta", PinType::Float, Value::F(1))
        .in("Min", PinType::Float, Value::F(-1e9f)).in("Max", PinType::Float, Value::F(1e9f))
        .out("Value", PinType::Float).params("Flag").run([addFlag](NodeCtx& c) {
            return addFlag(c, c.node.p, c.inF(0), c.inF(1), c.inF(2));
        });
    b.emplace_back("AddFlagNamed", "Game");
    b.back().exec().in("Name", PinType::String).in("Delta", PinType::Float, Value::F(1))
        .in("Min", PinType::Float, Value::F(-1e9f)).in("Max", PinType::Float, Value::F(1e9f))
        .out("Value", PinType::Float).run([addFlag](NodeCtx& c) {
            return addFlag(c, c.inS(0), c.inF(1), c.inF(2), c.inF(3));
        });

    b.emplace_back("RequestCamera", "Game");
    b.back().exec().in("Blend", PinType::Float, Value::F(0)).params(
        "Camera name (empty = release)").run([](NodeCtx& c) {
        c.world.requestCamera(c.node.p, c.inF(0));
        return 0;
    });
    b.emplace_back("PlaySound", "Game");
    // The Clip input overrides the param when connected, so a weapon's fire
    // sound can come out of the same data row as its damage and rate.
    b.back().exec().in("Volume", PinType::Float, Value::F(1)).in("Clip", PinType::String)
        .params("Clip (.wav path)").run([](NodeCtx& c) {
            std::string clip = c.inS(1);
            if (clip.empty()) clip = c.node.p;
            if (clip.empty()) return 0;
            SoundId s = audioEngine().loadSound(c.comp.resolveAsset(clip));
            if (s >= 0) audioEngine().playOneShot(s, c.inF(0));
            return 0;
        });

    // Force the cursor locked or free, overriding the default (which follows
    // whichever camera rig is steering). A pause menu drawn over live gameplay
    // is the case that needs it: the first-person camera is still there asking
    // for the mouse, but the player needs a pointer to click Resume with.
    // Capture = -1 hands control back to the rigs.
    // 0 = frozen (pause), 1 = normal, 0.3 = slow motion. Scripts keep running
    // at any scale, so whatever set 0 can still set it back.
    b.emplace_back("SetTimeScale", "Game");
    b.back().exec().in("Scale", PinType::Float, Value::F(1)).run([](NodeCtx& c) {
        c.world.setTimeScale(c.inF(0));
        return 0;
    });

    b.emplace_back("SetMouseCapture", "Game");
    b.back().exec().in("Capture", PinType::Float, Value::F(1)).run([](NodeCtx& c) {
        float v = c.inF(0);
        if (v < 0.0f) c.world.clearMouseCapture();
        else c.world.setMouseCapture(v != 0.0f);
        return 0;
    });

    b.emplace_back("QuitGame", "Game");
    b.back().exec().run([](NodeCtx& c) {
        c.world.requestQuit();
        return 0;
    });
    // Level transition. Queued, not immediate: the app layer swaps the scene
    // between frames (see World::requestLoadScene), so the rest of this graph's
    // token still runs against a live world.
    b.emplace_back("LoadScene", "Game");
    b.back().exec().params("Map (assets/maps/*.json)").run([](NodeCtx& c) {
        if (!c.node.p.empty()) c.world.requestLoadScene(c.node.p);
        return 0;
    });

    // Restart the level that is running, whatever it is.
    b.emplace_back("ReloadScene", "Game");
    b.back().exec().run([](NodeCtx& c) {
        if (!c.world.currentScene().empty()) c.world.requestLoadScene(c.world.currentScene());
        return 0;
    });

    b.emplace_back("StartMission", "Game");
    b.back().exec().params("Mission id").run([](NodeCtx& c) {
        c.world.missions.startMission(c.node.p);
        return 0;
    });
    b.emplace_back("SaveGame", "Game");
    b.back().exec().params("Slot name").run([](NodeCtx& c) {
        std::string slot = c.node.p.empty() ? "quick" : c.node.p;
        c.world.requestSaveGame("Saves/" + slot + ".json");
        return 0;
    });
    b.emplace_back("LoadGame", "Game");
    b.back().exec().params("Slot name").run([](NodeCtx& c) {
        std::string slot = c.node.p.empty() ? "quick" : c.node.p;
        c.world.requestLoadGame("Saves/" + slot + ".json");
        return 0;
    });

    // ---- AI navigation ----
    b.emplace_back("AIMoveTo", "Game");
    b.back().exec().in("Agent", PinType::Entity).in("Target", PinType::Vec3).run(
        [](NodeCtx& c) {
            Entity* e = c.inE(0);
            if (auto* a = e ? e->getComponent<NavAgentComponent>() : nullptr)
                a->moveTo(c.inV(1));
            return 0;
        });
    b.emplace_back("AIStop", "Game");
    b.back().exec().in("Agent", PinType::Entity).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        if (auto* a = e ? e->getComponent<NavAgentComponent>() : nullptr) a->stop();
        return 0;
    });
    b.emplace_back("AIArrived", "Game");
    b.back().in("Agent", PinType::Entity).out("Arrived", PinType::Bool).out(
        "Moving", PinType::Bool).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        auto* a = e ? e->getComponent<NavAgentComponent>() : nullptr;
        c.out(0, Value::B(a && a->arrived()));
        c.out(1, Value::B(a && a->isMoving()));
        return kEnd;
    });

    // ---- data tables (assets/data/*.json) ----
    b.emplace_back("GetData", "Game");
    b.back().in("Row", PinType::String).in("Field", PinType::String)
        .out("Number", PinType::Float).out("Text", PinType::String)
        .out("Found", PinType::Bool).params("Table (assets/data/*.json)")
        .run([](NodeCtx& c) {
            const DataTable* t = c.comp.assets() ? c.comp.assets()->dataTable(c.node.p) : nullptr;
            const DataCell* cell = t ? t->cell(c.inS(0), c.inS(1)) : nullptr;
            c.out(0, Value::F(cell ? cell->asNumber() : 0.0f));
            c.out(1, Value::S(cell ? cell->asText() : std::string()));
            c.out(2, Value::B(cell != nullptr));
            return kEnd;
        });
    b.emplace_back("DataRowCount", "Game");
    b.back().out("Count", PinType::Float).params("Table (assets/data/*.json)")
        .run([](NodeCtx& c) {
            const DataTable* t = c.comp.assets() ? c.comp.assets()->dataTable(c.node.p) : nullptr;
            c.out(0, Value::F(t ? (float)t->rowCount() : 0.0f));
            return kEnd;
        });
    b.emplace_back("DataRowName", "Game");
    b.back().in("Index", PinType::Float).out("Name", PinType::String)
        .params("Table (assets/data/*.json)").run([](NodeCtx& c) {
            const DataTable* t = c.comp.assets() ? c.comp.assets()->dataTable(c.node.p) : nullptr;
            int i = (int)c.inF(0);
            c.out(0, Value::S(t && i >= 0 && i < t->rowCount() ? t->rows()[i].name
                                                               : std::string()));
            return kEnd;
        });

    b.emplace_back("TriggerDialogue", "Game");
    b.back().exec().in("Entity", PinType::Entity).run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        if (auto* dt = e ? e->getComponent<DialogueTriggerComponent>() : nullptr) {
            dt->player().start();
            c.world.setActiveDialogue(&dt->player());
        }
        return 0;
    });

    b.emplace_back("SetAnimParam", "Entity");
    b.back().exec().in("Entity", PinType::Entity).in("Value", PinType::Float).params(
        "Anim parameter").run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        auto find = [&](Entity* x) -> AnimatorComponent* {
            if (!x) return nullptr;
            if (auto* a = x->getComponent<AnimatorComponent>()) return a;
            for (Entity* ch : x->children())
                if (auto* a = ch->getComponent<AnimatorComponent>()) return a;
            return nullptr;
        };
        if (auto* a = find(e)) a->setParam(c.node.p, c.inF(1));
        return 0;
    });

    b.emplace_back("GetAnimParam", "Entity");
    b.back().in("Entity", PinType::Entity).out("Value", PinType::Float).params(
        "Anim parameter").run([](NodeCtx& c) {
        Entity* e = c.inE(0);
        float v = 0.0f;
        if (e) {
            if (auto* a = e->getComponent<AnimatorComponent>()) v = a->param(c.node.p);
            else
                for (Entity* ch : e->children())
                    if (auto* a = ch->getComponent<AnimatorComponent>()) { v = a->param(c.node.p); break; }
        }
        c.out(0, Value::F(v));
        return kEnd;
    });

    // ================= Debug =================
    b.emplace_back("DrawLine", "Debug");
    b.back().exec().in("From", PinType::Vec3).in("To", PinType::Vec3).in(
        "Color", PinType::Vec3, Value::V(Vec3(0, 1, 0))).run([](NodeCtx& c) {
        debugDraw().line(c.inV(0), c.inV(1), c.inV(2));
        return 0;
    });

    b.emplace_back("Log", "Debug");
    b.back().exec().in("Message", PinType::String).run([](NodeCtx& c) {
        AE_LOG("[Script] %s", c.inS(0).c_str());
        return 0;
    });

    std::vector<NodeDef> out;
    out.reserve(b.size());
    for (auto& x : b) out.push_back(std::move(x.d));
    return out;
}

} // namespace

const std::vector<NodeDef>& scriptNodeDefs() {
    static const std::vector<NodeDef> g = buildDefs();
    return g;
}

const NodeDef* scriptNodeDef(const std::string& type) {
    for (const auto& d : scriptNodeDefs())
        if (d.type == type) return &d;
    return nullptr;
}

// ---- NodeCtx --------------------------------------------------------------------
Entity& NodeCtx::self() { return comp.entity(); }

Value NodeCtx::in(int i) {
    if (i >= (int)node.in.size()) {
        return i < (int)def.dataIn.size() ? def.dataIn[i].def : Value{};
    }
    const DataIn& di = node.in[i];
    if (!di.fromNode.empty()) {
        int idx = comp.graph().indexOf(di.fromNode);
        if (idx >= 0 && depth < 24) return comp.evalDataOut(idx, di.fromOut, dt, depth + 1);
        return Value{};
    }
    if (di.hasLiteral) return di.literal;
    return i < (int)def.dataIn.size() ? def.dataIn[i].def : Value{};
}

std::string NodeCtx::inS(int i) { return in(i).asS(&world); }

Entity* NodeCtx::inE(int i) {
    Value v = in(i);
    if (v.type != PinType::Entity || v.ent == 0) return nullptr;
    return world.findById(v.ent);
}

// ---- (de)serialization ------------------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void scriptValueWrite(std::ostringstream& o, const Value& v) {
    switch (v.type) {
    case PinType::Vec3:
        o << "\"t\": \"Vec3\", \"v\": [" << v.v.x << ", " << v.v.y << ", " << v.v.z << "]";
        break;
    case PinType::Bool: o << "\"t\": \"Bool\", \"b\": " << (v.b ? "true" : "false"); break;
    case PinType::String: o << "\"t\": \"String\", \"s\": \"" << esc(v.s) << "\""; break;
    default: o << "\"t\": \"Float\", \"f\": " << v.f; break;
    }
}

Value scriptValueRead(const JsonValue& j) {
    Value v;
    const std::string* t = j.string("t");
    v.type = t ? pinTypeFromName(*t) : PinType::Float;
    v.f = (float)j.num("f", 0.0);
    v.b = j.flag("b", false);
    if (const std::string* s = j.string("s")) v.s = *s;
    if (const JsonValue* a = j.find("v"))
        if (a->size() == 3)
            v.v = Vec3((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number);
    return v;
}

bool saveScriptGraph(const ScriptGraph& g, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"scriptGraph\": 2,\n  \"variables\": [\n";
    for (size_t i = 0; i < g.variables.size(); ++i) {
        o << "    { \"name\": \"" << esc(g.variables[i].name) << "\", ";
        scriptValueWrite(o, g.variables[i].value);
        o << " }" << (i + 1 < g.variables.size() ? "," : "") << "\n";
    }
    o << "  ],\n  \"nodes\": [\n";
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        const ScriptNode& n = g.nodes[i];
        o << "    { \"id\": \"" << esc(n.id) << "\", \"type\": \"" << esc(n.type) << "\"";
        if (!n.p.empty()) o << ", \"p\": \"" << esc(n.p) << "\"";
        if (n.n != 0.0f) o << ", \"n\": " << n.n;
        o << ", \"exec\": [";
        for (size_t e = 0; e < n.execOut.size(); ++e)
            o << (e ? ", " : "") << "\"" << esc(n.execOut[e]) << "\"";
        o << "], \"in\": [";
        for (size_t d = 0; d < n.in.size(); ++d) {
            const DataIn& di = n.in[d];
            o << (d ? ", " : "");
            if (!di.fromNode.empty())
                o << "{ \"from\": \"" << esc(di.fromNode) << "\", \"out\": " << di.fromOut << " }";
            else if (di.hasLiteral) {
                o << "{ ";
                scriptValueWrite(o, di.literal);
                o << " }";
            } else
                o << "{}";
        }
        o << "], \"x\": " << n.x << ", \"y\": " << n.y << " }";
        o << (i + 1 < g.nodes.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Script] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Script] saved %d nodes to %s", (int)g.nodes.size(), path.c_str());
    return f.good();
}

bool loadScriptGraph(ScriptGraph& g, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Script] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("nodes")) {
        AE_ERROR("[Script] malformed graph: %s", path.c_str());
        return false;
    }
    g = ScriptGraph{};
    g.version = root.integer("scriptGraph", 2);
    if (const JsonValue* vars = root.find("variables")) {
        for (size_t i = 0; i < vars->size(); ++i) {
            ScriptVariable v;
            if (const std::string* n = (*vars)[i].string("name")) v.name = *n;
            v.value = scriptValueRead((*vars)[i]);
            g.variables.push_back(std::move(v));
        }
    }
    const JsonValue& nodes = *root.find("nodes");
    for (size_t i = 0; i < nodes.size(); ++i) {
        const JsonValue& jn = nodes[i];
        ScriptNode n;
        if (const std::string* s = jn.string("id")) n.id = *s;
        if (const std::string* s = jn.string("type")) n.type = *s;
        if (const std::string* s = jn.string("p")) n.p = *s;
        n.n = (float)jn.num("n", 0.0);
        if (const JsonValue* ex = jn.find("exec"))
            for (size_t e = 0; e < ex->size(); ++e) n.execOut.push_back((*ex)[e].str);
        if (const JsonValue* di = jn.find("in")) {
            for (size_t d = 0; d < di->size(); ++d) {
                DataIn x;
                const JsonValue& j = (*di)[d];
                if (const std::string* from = j.string("from")) {
                    x.fromNode = *from;
                    x.fromOut = j.integer("out", 0);
                } else if (j.find("t")) {
                    x.hasLiteral = true;
                    x.literal = scriptValueRead(j);
                }
                n.in.push_back(std::move(x));
            }
        }
        n.x = (float)jn.num("x", 0.0);
        n.y = (float)jn.num("y", 0.0);
        // Pad pin arrays to the def so indexing is always safe.
        if (const NodeDef* def = scriptNodeDef(n.type)) {
            while (n.execOut.size() < def->execOut.size()) n.execOut.push_back("");
            while (n.in.size() < def->dataIn.size()) n.in.push_back({});
        } else {
            // A silent unknown node evaluates to nothing — make it loud.
            AE_WARN("[Script] %s: unknown node type '%s' (id '%s') — it will do nothing",
                    path.c_str(), n.type.c_str(), n.id.c_str());
        }
        g.nodes.push_back(std::move(n));
    }

    // Link validation — a typo'd id would silently go nowhere. Every dangling
    // exec/data link gets a loud file:node:pin message instead (agents grep
    // for these; --verify fails on them).
    std::map<std::string, const ScriptNode*> byId;
    for (const ScriptNode& n : g.nodes)
        if (!n.id.empty()) byId[n.id] = &n;
    for (const ScriptNode& n : g.nodes) {
        const NodeDef* def = scriptNodeDef(n.type);
        for (size_t e = 0; e < n.execOut.size(); ++e) {
            if (n.execOut[e].empty() || byId.count(n.execOut[e])) continue;
            const char* pin = def && e < def->execOut.size() ? def->execOut[e].c_str() : "exec";
            AE_WARN("[Script] %s: node '%s' (%s) exec pin %d ('%s') -> unknown node '%s'",
                    path.c_str(), n.id.c_str(), n.type.c_str(), (int)e, pin,
                    n.execOut[e].c_str());
        }
        for (size_t d = 0; d < n.in.size(); ++d) {
            const DataIn& x = n.in[d];
            if (x.fromNode.empty()) continue;
            const char* pin = def && d < def->dataIn.size() ? def->dataIn[d].name.c_str() : "in";
            auto src = byId.find(x.fromNode);
            if (src == byId.end()) {
                AE_WARN("[Script] %s: node '%s' (%s) input %d ('%s') <- unknown node '%s'",
                        path.c_str(), n.id.c_str(), n.type.c_str(), (int)d, pin,
                        x.fromNode.c_str());
                continue;
            }
            if (const NodeDef* srcDef = scriptNodeDef(src->second->type))
                if (x.fromOut < 0 || x.fromOut >= (int)srcDef->dataOut.size())
                    AE_WARN("[Script] %s: node '%s' (%s) input %d ('%s') <- node '%s' output "
                            "%d out of range (%s has %d outputs)",
                            path.c_str(), n.id.c_str(), n.type.c_str(), (int)d, pin,
                            x.fromNode.c_str(), x.fromOut, src->second->type.c_str(),
                            (int)srcDef->dataOut.size());
        }
    }
    return true;
}

// ---- runtime component --------------------------------------------------------------
void ScriptGraphComponent::onDeserialized(AssetLibrary& assets) {
    graph_ = ScriptGraph{};
    tokens_.clear();
    vars_.clear();
    onceDone_.clear();
    eventFired_.clear();
    prox_.clear();
    keyWas_.clear();
    eventOuts_.clear();
    mailbox_.clear();
    loopState.clear();
    started_ = false;
    projectRoot_ = assets.projectRoot();
    assets_ = &assets;
    if (graphPath.empty()) return;
    std::string rel = graphPath;
    if (!loadScriptGraph(graph_, assets.resolvePath(rel))) return;
    graphPath = rel;
    for (const auto& v : graph_.variables) vars_[v.name] = v.value;
}

std::string ScriptGraphComponent::resolveAsset(const std::string& rel) const {
    if (rel.size() > 1 && (rel[1] == ':' || rel[0] == '/')) return rel;
    return projectRoot_.empty() ? rel : projectRoot_ + "/" + rel;
}

Value ScriptGraphComponent::getVar(const std::string& name) const {
    auto it = vars_.find(name);
    return it != vars_.end() ? it->second : Value{};
}
void ScriptGraphComponent::setVar(const std::string& name, const Value& v) { vars_[name] = v; }

void ScriptGraphComponent::postEvent(const std::string& name, float value, uint32_t senderId) {
    if (mailbox_.size() >= 256) { // a feedback loop between two graphs, not a game
        AE_WARN("[Script] mailbox full on %s — event '%s' dropped", graphPath.c_str(),
                name.c_str());
        return;
    }
    mailbox_.push_back({name, value, senderId});
}

Value ScriptGraphComponent::evalDataOut(int nodeIdx, int outIdx, float dt, int depth) {
    const ScriptNode& n = graph_.nodes[nodeIdx];
    const NodeDef* def = scriptNodeDef(n.type);
    if (!def) return Value{};
    // Event outputs (e.g. OnUpdate's Dt) hold the values from the latest fire.
    if (def->isEvent || n.type == "Function") {
        auto it = eventOuts_.find(nodeIdx);
        if (it != eventOuts_.end() && outIdx < (int)it->second.size()) return it->second[outIdx];
        return Value{};
    }
    if (!def->pure()) {
        // Pulling data from an impure node re-runs it WITHOUT side effects only
        // if it has none; for safety return its last... v2 keeps it simple:
        // evaluate it (ForLoop Index is the common case, handled via loopState).
        if (n.type == "ForLoop") {
            // Latest index for any token (single-token loops in practice).
            for (auto& kv : loopState)
                if (kv.first.first == nodeIdx) return Value::F((float)(kv.second - 1));
            return Value::F(0);
        }
    }
    NodeCtx ctx{*this, world(), n, *def, dt, {}, -1.0f, depth};
    ctx.outs.assign(def->dataOut.size(), Value{});
    if (def->eval) def->eval(ctx);
    return outIdx < (int)ctx.outs.size() ? ctx.outs[outIdx] : Value{};
}

void ScriptGraphComponent::fireEvent(int nodeIndex, float dt, const std::vector<Value>& outs) {
    eventOuts_[nodeIndex] = outs;
    const ScriptNode& n = graph_.nodes[nodeIndex];
    if (n.execOut.empty() || n.execOut[0].empty()) return;
    int to = graph_.indexOf(n.execOut[0]);
    if (to < 0) return;
    if ((int)tokens_.size() >= 256) {
        AE_WARN("[Script] token cap reached in %s — event dropped", graphPath.c_str());
        return;
    }
    Token t;
    t.id = nextTokenId_++;
    t.node = to;
    step(t, dt);
    if (t.node >= 0) tokens_.push_back(std::move(t));
    while (!spawnQueue_.empty()) { // Sequence spawns from this event's chain
        Token s = std::move(spawnQueue_.front());
        spawnQueue_.erase(spawnQueue_.begin());
        step(s, dt);
        if (s.node >= 0) tokens_.push_back(std::move(s));
    }
}

void ScriptGraphComponent::step(Token& t, float dt) {
    for (int guard = 0; guard < 128 && t.node >= 0; ++guard) {
        const ScriptNode& n = graph_.nodes[t.node];
        const NodeDef* def = scriptNodeDef(n.type);
        if (!def) { t.node = -1; return; }

        auto follow = [&](int pin) {
            t.node = (pin >= 0 && pin < (int)n.execOut.size() && !n.execOut[pin].empty())
                         ? graph_.indexOf(n.execOut[pin])
                         : -1;
        };

        // ---- flow nodes that need token state (kept out of the lambdas) ----
        if (n.type == "Once") {
            int idx = t.node;
            if (onceDone_.count(idx)) { t.node = -1; return; }
            onceDone_.insert(idx);
            follow(0);
            continue;
        }
        if (n.type == "Sequence") {
            // Spawn parallel tokens for b/c (queued; tokens_ may be iterating),
            // continue this one along a.
            for (int pin = 1; pin <= 2; ++pin) {
                if (pin < (int)n.execOut.size() && !n.execOut[pin].empty()) {
                    int to = graph_.indexOf(n.execOut[pin]);
                    if (to >= 0 && (int)(tokens_.size() + spawnQueue_.size()) < 256) {
                        Token sub;
                        sub.id = nextTokenId_++;
                        sub.node = to;
                        spawnQueue_.push_back(std::move(sub));
                    }
                }
            }
            follow(0);
            continue;
        }
        if (n.type == "ForLoop") {
            auto key = std::make_pair(t.node, t.id);
            NodeCtx ctx{*this, world(), n, *def, dt, {}, -1.0f, 0};
            int count = (int)ctx.inF(0);
            int cur = loopState.count(key) ? loopState[key] : 0;
            if (cur < count) {
                loopState[key] = cur + 1;
                follow(0); // body (wire its end back here to iterate)
            } else {
                loopState.erase(key);
                follow(1); // done
            }
            continue;
        }
        if (n.type == "Call") {
            int fn = -1;
            for (size_t i = 0; i < graph_.nodes.size(); ++i)
                if (graph_.nodes[i].type == "Function" && graph_.nodes[i].p == n.p) { fn = (int)i; break; }
            if (fn < 0) { follow(0); continue; } // unknown function: skip through
            if (t.callStack.size() < 16) t.callStack.push_back({t.node, 0});
            const ScriptNode& f = graph_.nodes[fn];
            t.node = (!f.execOut.empty() && !f.execOut[0].empty()) ? graph_.indexOf(f.execOut[0]) : -1;
            if (t.node < 0 && !t.callStack.empty()) { // empty function: return at once
                auto ret = t.callStack.back();
                t.callStack.pop_back();
                const ScriptNode& caller = graph_.nodes[ret.first];
                t.node = (!caller.execOut.empty() && !caller.execOut[0].empty())
                             ? graph_.indexOf(caller.execOut[0]) : -1;
            }
            continue;
        }
        if (n.type == "Return") {
            if (!t.callStack.empty()) {
                auto ret = t.callStack.back();
                t.callStack.pop_back();
                const ScriptNode& caller = graph_.nodes[ret.first];
                t.node = (!caller.execOut.empty() && !caller.execOut[0].empty())
                             ? graph_.indexOf(caller.execOut[0]) : -1;
            } else {
                t.node = -1;
            }
            continue;
        }

        // ---- everything else runs through its registered eval ----
        NodeCtx ctx{*this, world(), n, *def, dt, {}, -1.0f, 0};
        ctx.outs.assign(def->dataOut.size(), Value{});
        int r = def->eval ? def->eval(ctx) : 0;
        if (r == kParked) {
            t.wait = ctx.parkSeconds > 0.0f ? ctx.parkSeconds : 0.0001f;
            t.resumeOut = 0;
            return; // parked on this node
        }
        if (r < 0) { t.node = -1; return; }
        follow(r);
    }
}

void ScriptGraphComponent::onStart() {
    started_ = true;
    for (size_t i = 0; i < graph_.nodes.size(); ++i)
        if (graph_.nodes[i].type == "OnStart") fireEvent((int)i, world().dt(), {});
}

void ScriptGraphComponent::onUpdate(float dt) {
    if (!started_ || graph_.nodes.empty()) { mailbox_.clear(); return; }
    World& w = world();

    // ---- deliver mail (SendEvent from other entities) ----
    // Swapped out first: an OnEvent handler that posts back to us must land in
    // the NEXT frame's mailbox, not this loop.
    if (!mailbox_.empty()) {
        std::vector<Mail> inbox;
        inbox.swap(mailbox_);
        for (const Mail& m : inbox)
            for (size_t i = 0; i < graph_.nodes.size(); ++i)
                if (graph_.nodes[i].type == "OnEvent" && graph_.nodes[i].p == m.name)
                    fireEvent((int)i, dt, {Value::F(m.value), Value::E(m.sender)});
    }

    // ---- poll events ----
    for (size_t i = 0; i < graph_.nodes.size(); ++i) {
        const ScriptNode& n = graph_.nodes[i];
        int idx = (int)i;
        if (n.type == "OnUpdate") {
            fireEvent(idx, dt, {Value::F(dt)});
        } else if (n.type == "OnKey") {
            if (n.p.empty()) continue;
            bool down = w.input().keys[(unsigned char)std::toupper((unsigned char)n.p[0])];
            bool& was = keyWas_[idx];
            if (down && !was) fireEvent(idx, dt, {});
            was = down;
        } else if (n.type == "OnPlayerNear") {
            std::string playerName = n.p.empty() ? "MainCamera" : n.p;
            Entity* player = w.find(playerName);
            if (!player) continue;
            Vec3 d = player->worldPosition() - entity().worldPosition();
            float r = n.n > 0.0f ? n.n : 3.0f;
            bool inside = dot(d, d) <= r * r;
            Prox& p = prox_[idx];
            if (inside && p == Prox::Outside) fireEvent(idx, dt, {Value::E(player->id())});
            p = inside ? Prox::Inside : Prox::Outside;
        } else if (n.type == "OnFlag") {
            if (eventFired_.count(idx)) continue;
            if (w.missions.flag(n.p) == (int)n.n) {
                eventFired_.insert(idx);
                fireEvent(idx, dt, {});
            }
        } else if (n.type == "OnUIButton") {
            for (const auto& id : w.uiEvents())
                if (id == n.p) fireEvent(idx, dt, {});
        } else if (n.type == "OnAction") {
            if (w.actions.pressed(n.p)) fireEvent(idx, dt, {});
        }
    }

    // ---- advance parked tokens ----
    for (auto& t : tokens_) {
        if (t.node < 0) continue;
        if (t.wait > 0.0f) {
            t.wait -= dt;
            if (t.wait > 0.0f) continue;
            const ScriptNode& n = graph_.nodes[t.node];
            t.node = (t.resumeOut < (int)n.execOut.size() && !n.execOut[t.resumeOut].empty())
                         ? graph_.indexOf(n.execOut[t.resumeOut])
                         : -1;
            t.wait = 0.0f;
        }
        step(t, dt);
    }
    tokens_.erase(std::remove_if(tokens_.begin(), tokens_.end(),
                                 [](const Token& t) { return t.node < 0; }),
                  tokens_.end());

    // Drain Sequence spawns queued during the sweep, running each newly spawned
    // chain right away (they may park or complete).
    while (!spawnQueue_.empty()) {
        Token t = std::move(spawnQueue_.front());
        spawnQueue_.erase(spawnQueue_.begin());
        step(t, dt);
        if (t.node >= 0) tokens_.push_back(std::move(t));
    }
}

} // namespace ae
