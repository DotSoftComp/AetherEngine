// Component (de)serialization is fully registry/reflection-driven: each
// component's reflect() visits its fields (engine/reflect.h) and the
// ComponentRegistry maps "type" strings to factories — no per-type code here.
#include "scene_io.h"
#include "world.h"
#include "assets.h"
#include "component_registry.h"
#include "reflect.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>

namespace ae {

namespace {

std::string esc(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        default: out += c;
        }
    }
    return out;
}

std::string num(float v) {
    char buf[48];
    // 9 significant digits round-trips a float exactly. "%g" gives 6, which is
    // fine to read but loses the low bits — and anything the loader normalizes
    // (a sun direction, a quaternion) then comes back a hair different, so a
    // save/load/save cycle never settles and the verify round-trip flags drift
    // on a scene nobody touched.
    std::snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

std::string vec3s(const Vec3& v) {
    return "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "]";
}
std::string vec4s(const Vec4& v) {
    return "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ", " + num(v.w) + "]";
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

// ---- reflection visitors -----------------------------------------------------

// Serializes a component to its inline JSON object by replaying reflect().
// Field order in reflect() defines the file layout (kept byte-stable with the
// previous hand-written writers). With forClone, skipOnClone fields (e.g. the
// default-camera flag) are omitted so the copy keeps its default.
class JsonWriteVisitor : public PropertyVisitor {
public:
    explicit JsonWriteVisitor(const char* type, bool forClone = false) : forClone_(forClone) {
        o_ << "{ \"type\": \"" << type << "\"";
    }
    std::string str() const { return o_.str() + " }"; }

    void visit(const char* key, float& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": " << num(v);
    }
    void visit(const char* key, int& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": " << v;
    }
    void visit(const char* key, bool& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": " << (v ? "true" : "false");
    }
    void visit(const char* key, std::string& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": \"" << esc(v) << "\"";
    }
    void visit(const char* key, Vec3& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": " << vec3s(v);
    }
    void visit(const char* key, Vec4& v, const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << key << "\": " << vec4s(v);
    }
    void visit(const char* guidKey, const char* nameKey, EntityRef& ref, std::string& legacyName,
               const PropMeta& m) override {
        if (skip(m)) return;
        sep(); o_ << "\"" << guidKey << "\": \"" << ref.guid.toString() << "\"";
        sep(); o_ << "\"" << nameKey << "\": \"" << esc(legacyName) << "\"";
    }
    void beginGroup(const char* key) override {
        sep(); o_ << "\"" << key << "\": { ";
        needComma_ = false;
    }
    void endGroup() override {
        o_ << " }";
        needComma_ = true;
    }

private:
    bool skip(const PropMeta& m) const { return forClone_ && m.skipOnClone; }
    void sep() {
        if (needComma_) o_ << ", ";
        else needComma_ = true;
    }
    std::ostringstream o_;
    bool needComma_ = true; // "type" is already written
    bool forClone_ = false;
};

// Fills a component's fields from its JSON object by replaying reflect().
// Missing keys keep the field's current (default) value.
class JsonReadVisitor : public PropertyVisitor {
public:
    JsonReadVisitor(const JsonValue& j, Entity* e) : e_(e) { stack_.push_back(&j); }

    void visit(const char* key, float& v, const PropMeta&) override {
        v = (float)cur().num(key, v);
    }
    void visit(const char* key, int& v, const PropMeta&) override { v = cur().integer(key, v); }
    void visit(const char* key, bool& v, const PropMeta&) override { v = cur().flag(key, v); }
    void visit(const char* key, std::string& v, const PropMeta&) override {
        if (const std::string* s = cur().string(key)) v = *s;
    }
    void visit(const char* key, Vec3& v, const PropMeta&) override {
        v = readVec3(cur().find(key), v);
    }
    void visit(const char* key, Vec4& v, const PropMeta&) override {
        v = readVec4(cur().find(key), v);
    }
    void visit(const char* guidKey, const char* nameKey, EntityRef& ref, std::string& legacyName,
               const PropMeta&) override {
        // Prefer the durable guid; fall back to the legacy name reference.
        // Runs in the component pass, after every entity already exists.
        if (const std::string* n = cur().string(nameKey)) legacyName = *n;
        if (const std::string* g = cur().string(guidKey)) {
            Guid guid = Guid::fromString(*g);
            if (guid.valid()) {
                ref.guid = guid;
                ref.cached = e_->world().findByGuid(guid);
                return;
            }
        }
        if (!legacyName.empty())
            if (Entity* t = e_->world().find(legacyName)) ref.set(t);
    }
    void beginGroup(const char* key) override {
        static const JsonValue kMissing;
        const JsonValue* g = cur().find(key);
        stack_.push_back(g ? g : &kMissing);
    }
    void endGroup() override { stack_.pop_back(); }

private:
    const JsonValue& cur() const { return *stack_.back(); }
    std::vector<const JsonValue*> stack_;
    Entity* e_;
};

// Rewrites EntityRef fields through a Guid remap (prefab instancing gives
// every spawned entity a fresh Guid). Works for any reflected component.
class RefRemapVisitor : public PropertyVisitor {
public:
    RefRemapVisitor(World& world, const std::map<Guid, Guid>& remap)
        : world_(world), remap_(remap) {}

    void visit(const char*, float&, const PropMeta&) override {}
    void visit(const char*, int&, const PropMeta&) override {}
    void visit(const char*, bool&, const PropMeta&) override {}
    void visit(const char*, std::string&, const PropMeta&) override {}
    void visit(const char*, Vec3&, const PropMeta&) override {}
    void visit(const char*, Vec4&, const PropMeta&) override {}
    void visit(const char*, const char*, EntityRef& ref, std::string&,
               const PropMeta&) override {
        auto it = remap_.find(ref.guid);
        if (it != remap_.end()) {
            ref.guid = it->second;
            ref.cached = world_.findByGuid(it->second);
        }
    }

private:
    World& world_;
    const std::map<Guid, Guid>& remap_;
};

// Returns false for component types with no typeName (skipped with a note).
bool writeComponent(std::ostringstream& o, Component* c) {
    const char* type = c->typeName();
    if (!type || !*type) return false;
    JsonWriteVisitor v(type);
    c->reflect(v);
    o << v.str();
    return true;
}

void readComponent(const JsonValue& j, Entity* e, AssetLibrary& assets) {
    const std::string* type = j.string("type");
    if (!type) return;
    const ComponentDesc* desc = componentRegistry().find(*type);
    if (!desc) {
        AE_WARN("[Scene] unknown component type '%s' on entity '%s' — skipped (typo, or its "
                "module/plugin is disabled? see Docs/reference/components.md)",
                type->c_str(), e->name().c_str());
        return;
    }
    Component* c = desc->create(*e);
    JsonReadVisitor v(j, e);
    c->reflect(v);
    c->onDeserialized(assets);
}

// DFS from roots so parents always precede children in the saved array.
// Roots and children are pushed reversed so spawn order is preserved.
void gatherDepthFirst(const World& world, std::vector<Entity*>& out) {
    std::vector<Entity*> stack;
    const auto& all = world.entities();
    for (auto it = all.rbegin(); it != all.rend(); ++it)
        if (!(*it)->parent()) stack.push_back(it->get());
    while (!stack.empty()) {
        Entity* e = stack.back();
        stack.pop_back();
        out.push_back(e);
        for (auto it = e->children().rbegin(); it != e->children().rend(); ++it)
            stack.push_back(*it);
    }
}

void gatherSubtree(Entity* root, std::vector<Entity*>& out) {
    out.push_back(root);
    for (Entity* c : root->children()) gatherSubtree(c, out);
}

// Writes one entity as a JSON object into `o`. `parentIndex` is its index in
// the enclosing array (-1 for a root). Shared by saveWorld and prefab saving.
void writeEntity(std::ostringstream& o, Entity* e, int parentIndex,
                 const std::map<const Entity*, int>& indexOf) {
    o << "    { \"name\": \"" << esc(e->name()) << "\", \"guid\": \"" << e->guid().toString()
      << "\", \"parent\": " << parentIndex
      << ", \"active\": " << (e->active() ? "true" : "false") << ",\n"
      << "      \"position\": " << vec3s(e->transform.position)
      << ", \"rotation\": " << vec4s(e->transform.rotation)
      << ", \"scale\": " << vec3s(e->transform.scaling) << ",\n"
      << "      \"components\": [";
    bool first = true;
    for (const auto& c : e->components()) {
        std::ostringstream co;
        if (writeComponent(co, c.get())) {
            o << (first ? "\n        " : ",\n        ") << co.str();
            first = false;
        } else {
            AE_WARN("[Scene] entity '%s' has a non-serializable component (skipped)",
                    e->name().c_str());
        }
    }
    o << (first ? "] }" : "\n      ] }");
}

// Post-load fixup: rewrite an entity's EntityRef fields through a Guid remap
// (used when a prefab is instantiated with fresh Guids so intra-prefab
// references point at the new copies, not the authored originals).
void remapEntityRefs(World& world, Entity* e, const std::map<Guid, Guid>& remap) {
    RefRemapVisitor v(world, remap);
    for (const auto& c : e->components()) c->reflect(v);
}

// Spawns an "entities" JSON array under `rootParent`. With freshGuids, every
// entity gets a new Guid and intra-array references are remapped afterwards
// (prefab instancing); otherwise stored Guids are preserved (scene load).
// Returns the spawned entities in array order.
std::vector<Entity*> spawnEntityArray(World& world, AssetLibrary& assets, const JsonValue& ents,
                                      Entity* rootParent, bool freshGuids) {
    std::vector<Entity*> byIndex;
    std::map<Guid, Guid> remap;
    byIndex.reserve(ents.size());

    for (size_t i = 0; i < ents.size(); ++i) {
        const JsonValue& ej = ents[i];
        const std::string* name = ej.string("name");
        int parent = ej.integer("parent", -1);
        Entity* p = (parent >= 0 && parent < (int)byIndex.size()) ? byIndex[parent] : rootParent;
        Guid stored;
        if (const std::string* g = ej.string("guid")) stored = Guid::fromString(*g);
        Guid use = freshGuids ? Guid::generate() : stored;
        if (freshGuids && stored.valid()) remap[stored] = use;
        Entity* e = world.spawnWithGuid(name ? *name : "Entity", use, p);
        e->setActive(ej.flag("active", true));
        e->transform.position = readVec3(ej.find("position"), Vec3(0, 0, 0));
        e->transform.rotation = readVec4(ej.find("rotation"), quatIdentity());
        e->transform.scaling = readVec3(ej.find("scale"), Vec3(1, 1, 1));
        byIndex.push_back(e);
    }
    for (size_t i = 0; i < ents.size(); ++i) {
        const JsonValue* comps = ents[i].find("components");
        if (!comps) continue;
        for (size_t c = 0; c < comps->size(); ++c)
            readComponent((*comps)[c], byIndex[i], assets);
    }
    if (freshGuids)
        for (Entity* e : byIndex) remapEntityRefs(world, e, remap);
    return byIndex;
}

} // namespace

std::string serializeWorld(const World& world, AssetLibrary& assets) {
    (void)assets;
    std::vector<Entity*> order;
    gatherDepthFirst(world, order);
    std::map<const Entity*, int> indexOf;
    for (size_t i = 0; i < order.size(); ++i) indexOf[order[i]] = (int)i;

    std::ostringstream o;
    o << "{\n  \"version\": 1,\n";
    o << "  \"environment\": { \"sunDir\": " << vec3s(world.env.sunDir)
      << ", \"skyIntensity\": " << num(world.env.skyIntensity) << " },\n";
    o << "  \"entities\": [\n";
    for (size_t i = 0; i < order.size(); ++i) {
        Entity* e = order[i];
        int parent = e->parent() ? indexOf[e->parent()] : -1;
        writeEntity(o, e, parent, indexOf);
        o << (i + 1 < order.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

bool deserializeWorldJson(World& world, AssetLibrary& assets, const JsonValue& root) {
    const JsonValue* ents = root.find("entities");
    if (!ents) {
        AE_ERROR("[Scene] no \"entities\" array");
        return false;
    }
    world.clear();
    if (const JsonValue* env = root.find("environment")) {
        // Normalize only when the authored vector actually needs it. Running
        // normalize() over an already-unit vector is not idempotent in float,
        // so doing it unconditionally means every save/load nudges the sun and
        // the round-trip check never converges.
        Vec3 sun = readVec3(env->find("sunDir"), world.env.sunDir);
        float len2 = dot(sun, sun);
        world.env.sunDir = std::fabs(len2 - 1.0f) > 1e-6f ? normalize(sun) : sun;
        world.env.skyIntensity = (float)env->num("skyIntensity", world.env.skyIntensity);
        world.env.fogDensity = (float)env->num("fogDensity", -1.0);
        world.env.fogHeightFalloff = (float)env->num("fogHeightFalloff", -1.0);
        world.env.fogColor = readVec3(env->find("fogColor"), Vec3(-1, -1, -1));
        world.env.volumetricIntensity = (float)env->num("volumetric", -1.0);
    }
    // Spawn preserving the authored Guids so serialized references resolve.
    spawnEntityArray(world, assets, *ents, nullptr, false);
    return true;
}

bool deserializeWorld(World& world, AssetLibrary& assets, const std::string& text) {
    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) {
        AE_ERROR("[Scene] malformed JSON");
        return false;
    }
    return deserializeWorldJson(world, assets, root);
}

bool saveWorld(const World& world, AssetLibrary& assets, const std::string& path) {
    std::string text = serializeWorld(world, assets);
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Scene] cannot write %s", path.c_str());
        return false;
    }
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Scene] saved %d entities to %s", (int)world.entities().size(), path.c_str());
    return f.good();
}

bool loadWorld(World& world, AssetLibrary& assets, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Scene] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    if (!deserializeWorld(world, assets, text)) {
        AE_ERROR("[Scene] failed to load %s", path.c_str());
        return false;
    }
    AE_LOG("[Scene] loaded %d entities from %s", (int)world.entities().size(), path.c_str());
    return true;
}

bool processSceneRequest(World& world, AssetLibrary& assets) {
    std::string map = world.takeLoadSceneRequest();
    if (map.empty()) return false;
    // Flags are the campaign's memory (keys carried, levels cleared, score), so
    // they outlive the map the way a save-game would; loadWorld resets the rest.
    std::map<std::string, int> carried = world.missions.flags();
    if (!loadWorld(world, assets, assets.resolvePath(map))) return false;
    world.setCurrentScene(map);
    for (const auto& kv : carried) world.missions.setFlag(kv.first, kv.second);
    return true;
}

// ---- duplication -------------------------------------------------------------

namespace {

// Deep copy through the reflection path: serialize each component to JSON and
// read it back onto the destination — the exact save/load round-trip, so any
// registered component (including game-module scripts) clones correctly.
// skipOnClone fields (default-camera flag) are dropped by the writer.
void cloneComponents(Entity* dst, Entity* src, AssetLibrary& assets) {
    for (const auto& c : src->components()) {
        const char* type = c->typeName();
        if (!type || !*type) continue;
        const ComponentDesc* desc = componentRegistry().find(type);
        if (!desc) continue;
        JsonWriteVisitor wv(type, /*forClone=*/true);
        c->reflect(wv);
        std::string text = wv.str();
        JsonValue j;
        if (!jsonParse(text.c_str(), text.size(), j)) continue;
        Component* n = desc->create(*dst);
        JsonReadVisitor rv(j, dst);
        n->reflect(rv);
        n->onDeserialized(assets);
    }
}

Entity* cloneRecursive(World& world, AssetLibrary& assets, Entity* src, Entity* parent,
                       const std::string& name) {
    Entity* dst = world.spawn(name, parent);
    dst->transform = src->transform;
    dst->setActive(src->active());
    cloneComponents(dst, src, assets);
    for (Entity* child : src->children())
        cloneRecursive(world, assets, child, dst, child->name());
    return dst;
}

} // namespace

Entity* duplicateEntity(World& world, AssetLibrary& assets, Entity* src) {
    if (!src) return nullptr;
    return cloneRecursive(world, assets, src, src->parent(), src->name() + " Copy");
}

// ---- agent bridge / tooling ---------------------------------------------------

std::string serializeEntitySubtree(const Entity& root) {
    std::vector<Entity*> order;
    gatherSubtree(const_cast<Entity*>(&root), order);
    std::map<const Entity*, int> indexOf;
    for (size_t i = 0; i < order.size(); ++i) indexOf[order[i]] = (int)i;

    std::ostringstream o;
    o << "{\n  \"prefab\": true,\n  \"root\": \"" << esc(root.name()) << "\",\n";
    o << "  \"entities\": [\n";
    for (size_t i = 0; i < order.size(); ++i) {
        // Parent index is relative to this subtree; the root's parent is -1.
        Entity* p = order[i]->parent();
        int parent = -1;
        if (p) {
            auto it = indexOf.find(p);
            if (it != indexOf.end()) parent = it->second;
        }
        writeEntity(o, order[i], parent, indexOf);
        o << (i + 1 < order.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";
    return o.str();
}

bool applyComponentJson(Entity& e, AssetLibrary& assets, const JsonValue& comp) {
    const std::string* type = comp.string("type");
    if (!type) return false;
    const ComponentDesc* desc = componentRegistry().find(*type);
    if (!desc) return false;
    Component* target = nullptr;
    for (const auto& c : e.components())
        if (c->typeName() && *type == c->typeName()) {
            target = c.get();
            break;
        }
    if (!target) target = desc->create(e);
    JsonReadVisitor v(comp, &e);
    target->reflect(v);
    target->onDeserialized(assets);
    return true;
}

Entity* spawnEntitiesFromJson(World& world, AssetLibrary& assets, const JsonValue& entities,
                              Entity* parent) {
    if (entities.type != JsonValue::Array || entities.size() == 0) return nullptr;
    std::vector<Entity*> spawned = spawnEntityArray(world, assets, entities, parent, true);
    return spawned.empty() ? nullptr : spawned.front();
}

// ---- prefabs ----------------------------------------------------------------

bool saveEntityPrefab(const Entity& root, AssetLibrary& assets, const std::string& path) {
    (void)assets;
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Prefab] cannot write %s", path.c_str());
        return false;
    }
    std::string text = serializeEntitySubtree(root);
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Prefab] saved '%s' to %s", root.name().c_str(), path.c_str());
    return f.good();
}

Entity* instantiatePrefab(World& world, AssetLibrary& assets, const std::string& path,
                          Entity* parent) {
    // Accept a project-relative path ("assets/entities/imp.json") as well as an
    // absolute one: scripts and scene files reference prefabs the same way they
    // reference every other asset, and resolvePath passes absolutes through.
    std::string abs = assets.resolvePath(path);
    std::ifstream f(abs, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Prefab] cannot open %s", path.c_str());
        return nullptr;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) {
        AE_ERROR("[Prefab] malformed JSON: %s", path.c_str());
        return nullptr;
    }
    const JsonValue* ents = root.find("entities");
    if (!ents || ents->size() == 0) {
        AE_ERROR("[Prefab] no entities in %s", path.c_str());
        return nullptr;
    }
    // Fresh Guids so each instance is unique; refs remapped to the new copies.
    std::vector<Entity*> spawned = spawnEntityArray(world, assets, *ents, parent, true);
    AE_LOG("[Prefab] instantiated %s (%d entities)", path.c_str(), (int)spawned.size());
    return spawned.empty() ? nullptr : spawned.front();
}

} // namespace ae
