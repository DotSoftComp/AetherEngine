// Agent-bridge command dispatcher — the editor half of editor/agent_bridge.h.
//
// Runs on the main thread (AgentBridge::pump inside Editor::frame), so every
// handler may touch the World/renderer directly, exactly like a menu action.
// Mutating commands record undo steps in edit mode; during Play they hit the
// live session and are discarded by the PIE restore on play.stop, mirroring
// what hand edits do.
#include "editor.h"
#include "../engine/scene_io.h"
#include "../engine/component_registry.h"
#include "../engine/agent_bridge_help.h"
#include "../core/log.h"
#include "../core/capture.h"
#include "../core/paths.h"
#include "../rhi/rhi.h"
#include <cstdio>

namespace ae {

namespace {

std::string jesc(const std::string& s) { return pulseJsonEscape(s); }
std::string ok(const std::string& rawResult) {
    return "{\"ok\":true,\"result\":" + rawResult + "}";
}
std::string fail(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + jesc(msg) + "\"}";
}

std::string numJson(float v) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return buf;
}
std::string vec3Json(const Vec3& v) {
    return "[" + numJson(v.x) + "," + numJson(v.y) + "," + numJson(v.z) + "]";
}
std::string vec4Json(const Vec4& v) {
    return "[" + numJson(v.x) + "," + numJson(v.y) + "," + numJson(v.z) + "," + numJson(v.w) + "]";
}

Vec3 readVec3(const JsonValue* a, Vec3 def) {
    if (!a || a->size() < 3) return def;
    return Vec3((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number);
}
Vec4 readVec4(const JsonValue* a, Vec4 def) {
    if (!a || a->size() < 4) return def;
    return Vec4((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number,
                (float)(*a)[3].number);
}

std::string entitySummary(const Entity* e) {
    return "{\"guid\":\"" + e->guid().toString() + "\",\"id\":" + std::to_string(e->id()) +
           ",\"name\":\"" + jesc(e->name()) + "\",\"position\":" +
           vec3Json(e->transform.position) + "}";
}

const char* levelName(LogLevel l) {
    return l == LogLevel::Error ? "error" : l == LogLevel::Warn ? "warn" : "info";
}



} // namespace

void Editor::startAgentBridge() {
    if (bridge_.running()) return;
    if (bridge_.start(bridgeConfig_.bridgePort, bridgeConfig_.bridgeToken))
        AE_LOG("[Bridge] PulseLABS agent bridge on 127.0.0.1:%d (token in "
               "%%APPDATA%%/AetherEngine/pulse.json)",
               bridgeConfig_.bridgePort);
    else
        AE_WARN("[Bridge] port %d unavailable (another editor running?) — bridge off",
                bridgeConfig_.bridgePort);
}

void Editor::stopAgentBridge() {
    if (!bridge_.running()) return;
    bridge_.stop();
    AE_LOG("[Bridge] agent bridge stopped");
}

void Editor::pumpAgentBridge() {
    if (!bridge_.running()) return;
    bridge_.pump([this](const std::string& method, const JsonValue& params) {
        return handleBridgeCommand(method, params);
    });
}

std::string Editor::handleBridgeCommand(const std::string& method, const JsonValue& params) {
    // guid first (durable), then name, then session id — same order the
    // scene loader resolves references.
    auto findBySelector = [this](const JsonValue& v) -> Entity* {
        if (v.type == JsonValue::String) {
            Guid g = Guid::fromString(v.str);
            if (g.valid())
                if (Entity* e = world_->findByGuid(g)) return e;
            return world_->find(v.str);
        }
        if (v.type != JsonValue::Object) return nullptr;
        if (const std::string* gs = v.string("guid")) {
            Guid g = Guid::fromString(*gs);
            if (g.valid())
                if (Entity* e = world_->findByGuid(g)) return e;
        }
        if (const std::string* n = v.string("name"))
            if (Entity* e = world_->find(*n)) return e;
        if (const JsonValue* id = v.find("id"))
            if (id->type == JsonValue::Number) return world_->findById((uint32_t)id->number);
        return nullptr;
    };
    auto findEntity = [&]() -> Entity* { return findBySelector(params); };

    // Shared by entity.spawn / entity.set: transform + name + active from params.
    auto applyEntityProps = [&](Entity* e) {
        // "name" is the selector in entity.set, so renaming uses its own key.
        if (const std::string* n = params.string("rename")) e->setName(*n);
        if (const JsonValue* a = params.find("active"))
            if (a->type == JsonValue::Bool) e->setActive(a->boolean);
        e->transform.position = readVec3(params.find("position"), e->transform.position);
        e->transform.rotation = readVec4(params.find("rotation"), e->transform.rotation);
        e->transform.scaling = readVec3(params.find("scale"), e->transform.scaling);
        if (const JsonValue* eu = params.find("rotationEuler")) {
            Vec3 d = readVec3(eu, Vec3(0, 0, 0));
            Vec4 q = quatMul(quatAxisAngle(Vec3(0, 1, 0), radians(d.y)),
                             quatMul(quatAxisAngle(Vec3(1, 0, 0), radians(d.x)),
                                     quatAxisAngle(Vec3(0, 0, 1), radians(d.z))));
            e->transform.rotation = q;
        }
    };

    if (method == "bridge.help") return ok(agentBridgeHelpJson());

    if (method == "status") {
        std::string scene = scenePath_.empty() ? "" : assets_->toProjectRelative(scenePath_);
        return ok("{\"project\":\"" + jesc(project_.name) + "\",\"projectRoot\":\"" +
                  jesc(projectRoot_) + "\",\"scene\":\"" + jesc(scene) +
                  "\",\"playing\":" + (playing_ ? "true" : "false") +
                  ",\"paused\":" + (paused_ ? "true" : "false") +
                  ",\"entities\":" + std::to_string(world_->entities().size()) +
                  ",\"fps\":" + numJson(fps_) +
                  ",\"compiling\":" + (compiling_ ? "true" : "false") + "}");
    }

    if (method == "registry.components") {
        std::string out = "[";
        bool first = true;
        for (const ComponentDesc& d : componentRegistry().all()) {
            out += std::string(first ? "" : ",") + "{\"type\":\"" + jesc(d.type) +
                   "\",\"displayName\":\"" + jesc(d.displayName) + "\",\"category\":\"" +
                   jesc(d.category) + "\"}";
            first = false;
        }
        return ok(out + "]");
    }

    if (method == "registry.assets") {
        auto arr = [](const std::vector<std::string>& names) {
            std::string a = "[";
            for (size_t i = 0; i < names.size(); ++i)
                a += (i ? ",\"" : "\"") + jesc(names[i]) + "\"";
            return a + "]";
        };
        return ok("{\"meshes\":" + arr(assets_->meshNames()) +
                  ",\"textureSets\":" + arr(assets_->textureSetNames()) + "}");
    }

    if (method == "world.get") return ok(serializeWorld(*world_, *assets_));

    if (method == "entity.list") {
        std::string out = "[";
        bool first = true;
        for (const auto& e : world_->entities()) {
            std::string comps = "[";
            bool cf = true;
            for (const auto& c : e->components()) {
                const char* t = c->typeName();
                if (!t || !*t) continue;
                comps += std::string(cf ? "\"" : ",\"") + t + "\"";
                cf = false;
            }
            out += std::string(first ? "" : ",") + "{\"guid\":\"" + e->guid().toString() +
                   "\",\"id\":" + std::to_string(e->id()) + ",\"name\":\"" + jesc(e->name()) +
                   "\",\"parentGuid\":" +
                   (e->parent() ? "\"" + e->parent()->guid().toString() + "\"" : "null") +
                   ",\"active\":" + (e->active() ? "true" : "false") +
                   ",\"position\":" + vec3Json(e->transform.position) +
                   ",\"componentTypes\":" + comps + "]}";
            first = false;
        }
        return ok(out + "]");
    }

    if (method == "entity.get") {
        Entity* e = findEntity();
        if (!e) return fail("entity not found (pass guid, name, or id)");
        return ok(serializeEntitySubtree(*e));
    }

    if (method == "entity.spawn") {
        Entity* parent = nullptr;
        if (const JsonValue* pj = params.find("parent")) {
            parent = findBySelector(*pj);
            if (!parent && pj->type != JsonValue::Null) return fail("parent not found");
        }
        Entity* e = nullptr;
        if (const std::string* kind = params.string("kind")) {
            e = spawnPrimitive(kind->c_str()); // records its own undo step
            if (!e) return fail("unknown kind '" + *kind +
                                "' (empty|cube|sphere|plane|torus|light|camera|trigger)");
            if (parent) world_->reparent(e, parent);
        } else if (const std::string* prefab = params.string("prefab")) {
            recordUndo();
            e = instantiatePrefab(*world_, *assets_, assets_->resolvePath(*prefab), parent);
            if (!e) return fail("prefab failed to load: " + *prefab);
        } else if (const JsonValue* ents = params.find("entities")) {
            recordUndo();
            e = spawnEntitiesFromJson(*world_, *assets_, *ents, parent);
            if (!e) return fail("params.entities must be a non-empty scene-shaped array");
        } else if (const std::string* name = params.string("name")) {
            recordUndo();
            e = world_->spawn(*name, parent);
        } else {
            return fail("pass kind, prefab, entities, or name");
        }
        if (const std::string* name = params.string("name")) e->setName(*name);
        applyEntityProps(e);
        if (const JsonValue* comps = params.find("components"))
            for (size_t i = 0; i < comps->size(); ++i)
                applyComponentJson(*e, *assets_, (*comps)[i]);
        select(e);
        AE_LOG("[Bridge] spawned '%s'", e->name().c_str());
        return ok(entitySummary(e));
    }

    if (method == "entity.set") {
        Entity* e = findEntity();
        if (!e) return fail("entity not found (pass guid, name, or id)");
        recordUndo();
        applyEntityProps(e);
        if (const JsonValue* pj = params.find("parent")) {
            Entity* parent = pj->type == JsonValue::Null ? nullptr : findBySelector(*pj);
            if (pj->type != JsonValue::Null && !parent) return fail("parent not found");
            if (!world_->reparent(e, parent)) return fail("reparent refused (cycle)");
        }
        markViewportDirty();
        return ok(entitySummary(e));
    }

    if (method == "entity.destroy") {
        Entity* e = findEntity();
        if (!e) return fail("entity not found (pass guid, name, or id)");
        recordUndo();
        std::string name = e->name();
        if (selected() == e) select(nullptr);
        world_->destroy(e); // deferred to the end of the next world update
        markViewportDirty();
        AE_LOG("[Bridge] destroyed '%s'", name.c_str());
        return ok("{\"destroyed\":\"" + jesc(name) + "\"}");
    }

    if (method == "component.set") {
        Entity* e = findEntity();
        if (!e) return fail("entity not found (pass guid, name, or id)");
        recordUndo();
        int applied = 0;
        auto apply = [&](const JsonValue& comp) {
            if (applyComponentJson(*e, *assets_, comp)) ++applied;
        };
        if (const JsonValue* one = params.find("component")) apply(*one);
        if (const JsonValue* many = params.find("components"))
            for (size_t i = 0; i < many->size(); ++i) apply((*many)[i]);
        if (!applied)
            return fail("no component applied — unknown type? (see registry.components)");
        markViewportDirty();
        return ok("{\"applied\":" + std::to_string(applied) + "}");
    }

    if (method == "component.remove") {
        Entity* e = findEntity();
        if (!e) return fail("entity not found (pass guid, name, or id)");
        const std::string* type = params.string("type");
        if (!type) return fail("pass params.type");
        for (const auto& c : e->components()) {
            if (!c->typeName() || *type != c->typeName()) continue;
            recordUndo();
            e->removeComponent(c.get());
            markViewportDirty();
            return ok("{\"removed\":\"" + jesc(*type) + "\"}");
        }
        return fail("entity has no '" + *type + "' component");
    }

    if (method == "scene.new") {
        if (playing_) return fail("stop Play first (play.stop)");
        newScene();
        return ok("{\"entities\":" + std::to_string(world_->entities().size()) + "}");
    }

    if (method == "scene.save") {
        if (playing_) return fail("stop Play first (play.stop) — the played state is transient");
        std::string path = scenePath_;
        if (const std::string* p = params.string("path")) path = assets_->resolvePath(*p);
        if (path.empty()) return fail("scene was never saved — pass params.path");
        if (!saveWorld(*world_, *assets_, path)) return fail("write failed: " + path);
        scenePath_ = path;
        return ok("{\"path\":\"" + jesc(path) + "\"}");
    }

    if (method == "scene.load") {
        if (playing_) return fail("stop Play first (play.stop)");
        const std::string* p = params.string("path");
        if (!p) return fail("pass params.path");
        std::string full = assets_->resolvePath(*p);
        if (!loadWorld(*world_, *assets_, full)) return fail("load failed: " + *p);
        scenePath_ = full;
        select(nullptr);
        primed_ = false;
        clearUndo();
        markViewportDirty();
        return ok("{\"entities\":" + std::to_string(world_->entities().size()) + "}");
    }

    if (method == "scene.environment") {
        bool set = params.find("sunDir") || params.find("skyIntensity");
        if (set) {
            recordUndo();
            world_->env.sunDir = normalize(readVec3(params.find("sunDir"), world_->env.sunDir));
            world_->env.skyIntensity =
                (float)params.num("skyIntensity", world_->env.skyIntensity);
            markViewportDirty();
        }
        return ok("{\"sunDir\":" + vec3Json(world_->env.sunDir) +
                  ",\"skyIntensity\":" + numJson(world_->env.skyIntensity) + "}");
    }

    if (method == "play.start" || method == "play.stop" || method == "play.pause") {
        if (method == "play.start") startPlay();
        else if (method == "play.stop") stopPlay();
        else if (playing_) {
            const JsonValue* p = params.find("paused");
            paused_ = p && p->type == JsonValue::Bool ? p->boolean : !paused_;
        }
        markViewportDirty();
        return ok(std::string("{\"playing\":") + (playing_ ? "true" : "false") +
                  ",\"paused\":" + (paused_ ? "true" : "false") + "}");
    }

    if (method == "logs.get") {
        const auto& entries = logEntries();
        size_t since = (size_t)params.num("since", 0);
        size_t max = (size_t)params.num("max", 200);
        if (max > 2000) max = 2000;
        std::string out = "{\"total\":" + std::to_string(entries.size()) + ",\"entries\":[";
        size_t i = since < entries.size() ? since : entries.size();
        for (size_t n = 0; i < entries.size() && n < max; ++i, ++n) {
            const LogEntry& le = entries[i];
            out += std::string(n ? "," : "") + "{\"i\":" + std::to_string(i) +
                   ",\"level\":\"" + levelName(le.level) + "\",\"time\":" +
                   numJson((float)le.time) + ",\"text\":\"" + jesc(le.text) + "\"}";
        }
        return ok(out + "],\"next\":" + std::to_string(i) + "}");
    }

    if (method == "logs.clear") {
        clearLog();
        return ok("{}");
    }

    if (method == "viewport.screenshot") {
        std::string path = joinPath(projectRoot_, "bridge_screenshot.bmp");
        if (const std::string* p = params.string("path")) path = assets_->resolvePath(*p);
        // Fresh transforms (a spawn earlier in this pump hasn't ticked yet),
        // then a real render into the viewport FBO.
        world_->update(0.0f, playing_ ? playClock_ : 0.0f, window_->input(), false);
        renderSceneToViewport(0.0f);
        std::vector<uint8_t> pixels((size_t)vpW_ * vpH_ * 4);
        rhi::readFramebuffer(vpFBO_, vpW_, vpH_, pixels.data());
        if (!writeBMP(path.c_str(), vpW_, vpH_, pixels)) return fail("write failed: " + path);
        return ok("{\"path\":\"" + jesc(path) + "\",\"width\":" + std::to_string(vpW_) +
                  ",\"height\":" + std::to_string(vpH_) + "}");
    }

    if (method == "camera.get" || method == "camera.set") {
        if (method == "camera.set") {
            camPos_ = readVec3(params.find("position"), camPos_);
            camYaw_ = (float)params.num("yaw", camYaw_);
            camPitch_ = (float)params.num("pitch", camPitch_);
            markViewportDirty();
        }
        return ok("{\"position\":" + vec3Json(camPos_) + ",\"yaw\":" + numJson(camYaw_) +
                  ",\"pitch\":" + numJson(camPitch_) + "}");
    }

    if (method == "compile.scripts") {
        if (!project_.hasModule()) return fail("project has no C++ Source module");
        if (compiling_) return fail("a compile is already running (compile.status)");
        startCompileScripts();
        return ok("{\"started\":true}");
    }

    if (method == "compile.status") {
        return ok(std::string("{\"compiling\":") + (compiling_ ? "true" : "false") +
                  ",\"lastOk\":" + (compileOk_ ? "true" : "false") + "}");
    }

    if (method == "nav.bake") {
        bool baked = world_->nav.bake(*world_);
        if (baked) showNavmesh_ = true;
        markViewportDirty();
        return ok(std::string("{\"baked\":") + (baked ? "true" : "false") + "}");
    }

    if (method == "edit.undo" || method == "edit.redo") {
        if (method == "edit.undo") {
            if (!canUndo()) return fail("nothing to undo");
            undo();
        } else {
            if (!canRedo()) return fail("nothing to redo");
            redo();
        }
        return ok("{\"undoSteps\":" + std::to_string(undoStack_.size()) +
                  ",\"redoSteps\":" + std::to_string(redoStack_.size()) + "}");
    }

    return fail("unknown method '" + method + "' — call bridge.help");
}

} // namespace ae
