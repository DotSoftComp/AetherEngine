#include "doc_gen.h"
#include "agent_bridge_help.h"
#include "component_registry.h"
#include "engine_modules.h"
#include "entity.h"
#include "reflect.h"
#include "world.h"
#include "../script/script_graph.h"
#include "../core/log.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ae {

namespace {

// Collects one row per reflected field: key, type, default, presentation meta.
class DocVisitor : public PropertyVisitor {
public:
    struct Row {
        std::string key, type, def, label, range;
    };
    std::vector<Row> rows;
    std::string group; // current beginGroup prefix ("material.")

    static std::string num(float v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%g", v);
        return b;
    }
    std::string range(const PropMeta& m) {
        if (m.min == m.max) return "";
        return num(m.min) + " .. " + num(m.max);
    }
    void add(const char* key, const char* type, std::string def, const PropMeta& m) {
        rows.push_back({group + key, type, std::move(def),
                        m.label ? std::string(m.label) : std::string(), range(m)});
    }

    void visit(const char* k, float& v, const PropMeta& m) override { add(k, "float", num(v), m); }
    void visit(const char* k, int& v, const PropMeta& m) override {
        add(k, "int", std::to_string(v), m);
    }
    void visit(const char* k, bool& v, const PropMeta& m) override {
        add(k, "bool", v ? "true" : "false", m);
    }
    void visit(const char* k, std::string& v, const PropMeta& m) override {
        add(k, "string", v.empty() ? "\"\"" : "\"" + v + "\"", m);
    }
    void visit(const char* k, Vec3& v, const PropMeta& m) override {
        add(k, "vec3", "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + "]", m);
    }
    void visit(const char* k, Vec4& v, const PropMeta& m) override {
        add(k, "vec4",
            "[" + num(v.x) + ", " + num(v.y) + ", " + num(v.z) + ", " + num(v.w) + "]", m);
    }
    void visit(const char* guidKey, const char* nameKey, EntityRef&, std::string&,
               const PropMeta& m) override {
        add(guidKey, "entity guid", "\"\"", m);
        add(nameKey, "entity name (fallback)", "\"\"", m);
    }
    void beginGroup(const char* key) override { group = std::string(key) + "."; }
    void endGroup() override { group.clear(); }
};

std::string moduleLabel(int moduleId) {
    if (moduleId == 0) return "core";
    if (moduleId == kGameModuleId) return "game module";
    if (moduleId >= kPluginModuleBase) return "plugin";
    for (const EngineModuleDesc& m : engineModules().all())
        if (m.moduleId == moduleId) return "module: " + m.id;
    return "module " + std::to_string(moduleId);
}

// Writes `content` to `path` only when it differs from what's already there, so
// regenerating on every project open never dirties unchanged files (no git
// noise). Sets `changed` = true iff the file was actually rewritten.
bool writeIfChanged(const std::string& path, const std::string& content, bool& changed) {
    changed = false;
    {
        std::ifstream in(path, std::ios::binary);
        if (in) {
            std::ostringstream ss;
            ss << in.rdbuf();
            if (ss.str() == content) return true; // identical — leave it alone
        }
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f << content;
    changed = f.good();
    return f.good();
}

bool writeComponentsDoc(const std::string& path, bool& changed) {
    std::ostringstream o;
    o << "# Component reference\n\n"
      << "GENERATED from the live component registry (AetherDocGen) - do not edit.\n"
      << "Every component serializes into a scene entity's `components` array as\n"
      << "`{ \"type\": \"<Type>\", <field>: <value>, ... }`. Field keys below are the\n"
      << "exact JSON keys; omitted fields keep their defaults. Ranges are editor\n"
      << "hints, not validation.\n\n";

    World world; // scratch host for one throwaway instance of each type
    Entity* host = world.spawn("docgen");

    for (const ComponentDesc& d : componentRegistry().all()) {
        Component* c = d.create ? d.create(*host) : nullptr;
        o << "## " << d.type << "\n\n";
        o << "- display name: " << d.displayName << "  \n";
        o << "- category: " << d.category << "  \n";
        o << "- source: " << moduleLabel(d.moduleId) << "\n\n";
        if (c) {
            DocVisitor v;
            c->reflect(v);
            if (!v.rows.empty()) {
                o << "| field | type | default | label | range |\n";
                o << "|---|---|---|---|---|\n";
                for (const auto& r : v.rows)
                    o << "| `" << r.key << "` | " << r.type << " | `" << r.def << "` | "
                      << r.label << " | " << r.range << " |\n";
                o << "\n";
            } else {
                o << "No serialized fields.\n\n";
            }
        }
    }

    return writeIfChanged(path, o.str(), changed);
}

bool writeScriptNodesDoc(const std::string& path, bool& changed) {
    std::ostringstream o;
    o << "# Script-graph node reference\n\n"
      << "GENERATED from the live node registry (AetherDocGen) - do not edit.\n"
      << "See Docs/script-graphs.md for the graph file schema. Node JSON shape:\n"
      << "`{ \"id\": \"<unique>\", \"type\": \"<Type>\", \"p\": \"<param>\", \"n\": <number>,\n"
      << "   \"exec\": [\"<next-id>\", ...], \"in\": [<input>, ...], \"x\": 0, \"y\": 0 }`\n"
      << "where each input is either a link `{ \"from\": \"<node-id>\", \"out\": <index> }`\n"
      << "or a literal (`{\"t\":\"Float\",\"f\":1}`, `{\"t\":\"Vec3\",\"v\":[x,y,z]}`,\n"
      << "`{\"t\":\"Bool\",\"b\":true}`, `{\"t\":\"String\",\"s\":\"text\"}`).\n"
      << "`exec` lists one target node id per exec-out pin (\"\" = end). Events have\n"
      << "no exec-in; pure nodes (no exec pins) evaluate on demand.\n\n";

    std::string lastCategory;
    for (const NodeDef& d : scriptNodeDefs()) {
        if (d.category != lastCategory) {
            o << "## " << d.category << "\n\n";
            lastCategory = d.category;
        }
        o << "### " << d.type << "\n\n";
        if (d.isEvent) o << "- event (no exec-in; fires on its own)\n";
        else if (d.pure()) o << "- pure (no exec pins; evaluated when an input pulls it)\n";
        if (!d.execOut.empty()) {
            o << "- exec out: ";
            for (size_t i = 0; i < d.execOut.size(); ++i)
                o << (i ? ", " : "") << "`" << d.execOut[i] << "`";
            o << "\n";
        }
        for (size_t i = 0; i < d.dataIn.size(); ++i) {
            const PinDef& p = d.dataIn[i];
            o << "- in " << i << ": `" << p.name << "` (" << pinTypeName(p.type) << ")";
            std::string def = p.def.asS(nullptr);
            if (!def.empty()) o << " default `" << def << "`";
            o << "\n";
        }
        for (size_t i = 0; i < d.dataOut.size(); ++i)
            o << "- out " << i << ": `" << d.dataOut[i].name << "` ("
              << pinTypeName(d.dataOut[i].type) << ")\n";
        if (d.pLabel) o << "- param `p` (string): " << d.pLabel << "\n";
        if (d.nLabel) o << "- param `n` (number): " << d.nLabel << "\n";
        o << "\n";
    }

    return writeIfChanged(path, o.str(), changed);
}

bool writeAgentBridgeDoc(const std::string& path, bool& changed) {
    std::ostringstream o;
    o << "# Agent bridge — driving a LIVE editor\n\n"
      << "GENERATED (AetherDocGen) - do not edit. Method list comes from the same\n"
      << "source the running editor serves via `bridge.help`.\n\n"
      << "While the Aether editor is open it hosts a control server on\n"
      << "`http://127.0.0.1:3052` (port = `bridgePort` in\n"
      << "`%APPDATA%/AetherEngine/pulse.json`). It gives full control of the live\n"
      << "editor: spawn and modify entities, start/stop Play, read logs, capture\n"
      << "viewport screenshots, trigger C++ compiles and navmesh bakes.\n\n"
      << "**Access is PulseLABS-gated**: every request must send the `bridgeToken`\n"
      << "from that same `pulse.json` as an `x-aether-token` header. Headless runs\n"
      << "(`--frames`, `--screenshot`, `--resave`) need no editor and no token —\n"
      << "prefer them when no editor is open; prefer the bridge when one is, so\n"
      << "changes are verified against the live session.\n\n"
      << "## Protocol\n\n"
      << "- `GET /health` — liveness probe, no auth: is an editor running?\n"
      << "- `POST /rpc` — body `{ \"method\": \"...\", \"params\": { ... } }`,\n"
      << "  returns `{ \"ok\": true, \"result\": ... }` or `{ \"ok\": false, \"error\": \"...\" }`.\n\n"
      << "```\n"
      << "curl -s -X POST http://127.0.0.1:3052/rpc -H \"x-aether-token: <bridgeToken>\" \\\n"
      << "  -d '{\"method\":\"entity.spawn\",\"params\":{\"kind\":\"cube\",\"name\":\"Crate\",\"position\":[0,2,0]}}'\n"
      << "```\n\n"
      << "Typical verification loop: `status` -> mutate (`entity.spawn`,\n"
      << "`component.set`) -> `viewport.screenshot` -> `play.start` -> `logs.get`\n"
      << "(grep `[Script]`) -> `play.stop`. Entity selectors accept `guid`, `name`,\n"
      << "or `id`; component fields use the exact JSON keys from\n"
      << "`reference/components.md`.\n\n"
      << "## Method reference (verbatim `bridge.help`)\n\n"
      << "```json\n"
      << agentBridgeHelpJson() << "\n"
      << "```\n";

    return writeIfChanged(path, o.str(), changed);
}

} // namespace

bool generateReferenceDocs(const std::string& docsDir) {
    CreateDirectoryA(docsDir.c_str(), nullptr);
    std::string refDir = docsDir + "\\reference";
    CreateDirectoryA(refDir.c_str(), nullptr);

    bool c1 = false, c2 = false, c3 = false;
    bool ok = writeComponentsDoc(refDir + "\\components.md", c1);
    ok = writeScriptNodesDoc(refDir + "\\script-nodes.md", c2) && ok;
    ok = writeAgentBridgeDoc(refDir + "\\agent-bridge.md", c3) && ok;
    int changed = (c1 ? 1 : 0) + (c2 ? 1 : 0) + (c3 ? 1 : 0);
    if (!ok) AE_ERROR("[Docs] reference generation failed (%s)", refDir.c_str());
    else if (changed) AE_LOG("[Docs] reference refreshed in %s (%d file(s) updated)",
                             refDir.c_str(), changed);
    else AE_LOG("[Docs] reference already current (%s)", refDir.c_str());
    return ok;
}

} // namespace ae
