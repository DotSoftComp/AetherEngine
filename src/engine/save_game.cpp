// Aether Engine — save-game serialization (see save_game.h).
#define WIN32_LEAN_AND_MEAN
#include "save_game.h"
#include "world.h"
#include "entity.h"
#include "assets.h"
#include "scene_io.h"
#include "anim_graph.h"
#include "../script/script_graph.h"
#include "../core/json.h"
#include "../core/log.h"
#include <windows.h> // CreateDirectoryA
#include <fstream>
#include <sstream>

namespace ae {

static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

bool saveGame(World& world, AssetLibrary& assets, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"saveGame\": 1,\n";

    // ---- story flags ----
    o << "  \"flags\": [";
    bool first = true;
    for (const auto& kv : world.missions.flags()) {
        o << (first ? "" : ", ") << "{ \"name\": \"" << esc(kv.first)
          << "\", \"value\": " << kv.second << " }";
        first = false;
    }
    o << "],\n";

    // ---- mission / objective states ----
    o << "  \"missions\": [";
    first = true;
    for (const auto& m : world.missions.missions) {
        o << (first ? "" : ", ") << "{ \"id\": \"" << esc(m.id)
          << "\", \"state\": " << (int)m.state << ", \"objectives\": [";
        for (size_t i = 0; i < m.objectives.size(); ++i)
            o << (i ? ", " : "") << (int)m.objectives[i].state;
        o << "] }";
        first = false;
    }
    o << "],\n";

    // ---- per-entity runtime state (script vars, anim params), keyed by GUID ----
    o << "  \"runtime\": [\n";
    first = true;
    for (const auto& e : world.entities()) {
        std::ostringstream re;
        bool any = false;
        for (const auto& c : e->components()) {
            if (auto* sg = dynamic_cast<ScriptGraphComponent*>(c.get())) {
                if (sg->vars().empty()) continue;
                re << (any ? ", " : "") << "\"scriptVars\": [";
                bool f2 = true;
                for (const auto& kv : sg->vars()) {
                    re << (f2 ? "" : ", ") << "{ \"name\": \"" << esc(kv.first) << "\", ";
                    scriptValueWrite(re, kv.second);
                    re << " }";
                    f2 = false;
                }
                re << "]";
                any = true;
            } else if (auto* an = dynamic_cast<AnimatorComponent*>(c.get())) {
                if (an->params().empty()) continue;
                re << (any ? ", " : "") << "\"animParams\": [";
                bool f2 = true;
                for (const auto& kv : an->params()) {
                    re << (f2 ? "" : ", ") << "{ \"name\": \"" << esc(kv.first)
                       << "\", \"value\": " << kv.second << " }";
                    f2 = false;
                }
                re << "]";
                any = true;
            }
        }
        if (!any) continue;
        o << (first ? "" : ",\n") << "    { \"guid\": \"" << e->guid().toString() << "\", "
          << re.str() << " }";
        first = false;
    }
    o << "\n  ],\n";

    // ---- the world itself (verbatim scene JSON) ----
    o << "  \"world\":\n" << serializeWorld(world, assets) << "}\n";

    // Ensure the Saves directory exists when saving into one.
    std::string dir = path;
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) CreateDirectoryA(dir.substr(0, slash).c_str(), nullptr);

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Save] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Save] game saved to %s", path.c_str());
    return f.good();
}

bool loadGame(World& world, AssetLibrary& assets, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[Save] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("saveGame")) {
        AE_ERROR("[Save] malformed save file: %s", path.c_str());
        return false;
    }
    const JsonValue* worldJ = root.find("world");
    if (!worldJ || !deserializeWorldJson(world, assets, *worldJ)) {
        AE_ERROR("[Save] save file has no valid world: %s", path.c_str());
        return false;
    }

    // Flags + missions on top of the freshly restored world.
    world.missions.resetRuntime();
    if (const JsonValue* flags = root.find("flags"))
        for (size_t i = 0; i < flags->size(); ++i) {
            const std::string* n = (*flags)[i].string("name");
            if (n) world.missions.setFlag(*n, (*flags)[i].integer("value", 0));
        }
    if (const JsonValue* ms = root.find("missions"))
        for (size_t i = 0; i < ms->size(); ++i) {
            const std::string* id = (*ms)[i].string("id");
            if (!id) continue;
            Mission* m = world.missions.find(*id);
            if (!m) continue;
            m->state = (MissionState)(*ms)[i].integer("state", 0);
            if (const JsonValue* obs = (*ms)[i].find("objectives"))
                for (size_t k = 0; k < obs->size() && k < m->objectives.size(); ++k)
                    m->objectives[k].state = (ObjectiveState)(int)(*obs)[k].number;
        }

    // Per-entity runtime state.
    if (const JsonValue* rt = root.find("runtime")) {
        for (size_t i = 0; i < rt->size(); ++i) {
            const std::string* g = (*rt)[i].string("guid");
            if (!g) continue;
            Entity* e = world.findByGuid(Guid::fromString(*g));
            if (!e) continue;
            if (const JsonValue* vars = (*rt)[i].find("scriptVars")) {
                if (auto* sg = e->getComponent<ScriptGraphComponent>())
                    for (size_t k = 0; k < vars->size(); ++k) {
                        const std::string* n = (*vars)[k].string("name");
                        if (n) sg->setVar(*n, scriptValueRead((*vars)[k]));
                    }
            }
            if (const JsonValue* ps = (*rt)[i].find("animParams")) {
                if (auto* an = e->getComponent<AnimatorComponent>())
                    for (size_t k = 0; k < ps->size(); ++k) {
                        const std::string* n = (*ps)[k].string("name");
                        if (n) an->setParam(*n, (float)(*ps)[k].num("value", 0.0));
                    }
            }
        }
    }
    AE_LOG("[Save] game loaded from %s", path.c_str());
    return true;
}

bool processSaveRequests(World& world, AssetLibrary& assets) {
    std::string savePath = world.takeSaveRequest();
    if (!savePath.empty()) saveGame(world, assets, assets.resolvePath(savePath));
    std::string loadPath = world.takeLoadRequest();
    if (!loadPath.empty()) return loadGame(world, assets, assets.resolvePath(loadPath));
    return false;
}

} // namespace ae
