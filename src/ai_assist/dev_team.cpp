#include "dev_team.h"
#include "../core/http.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../engine/component_registry.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

namespace { // defined below; used by the helpers here
std::string jsonToText(const JsonValue& v);
std::string readTextFile(const std::string& path);
}

// Local models paraphrase schema keys ("increment" for "title", "specialist"
// for "agent", "details" for "detail", ...). Read the first key that's present
// so parsing survives that drift instead of silently dropping the whole plan.
static std::string jsonFirstString(const JsonValue& j,
                                   std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (const std::string* s = j.string(k)) return *s;
    return {};
}
static const JsonValue* jsonFirstArray(const JsonValue& j,
                                       std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        if (const JsonValue* v = j.find(k))
            if (v->type == JsonValue::Array) return v;
    return nullptr;
}

static std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// A step field that is just a role word ("design"/"code"/...) is the agent, not
// the step's description — some models put the role in "step" and the task text
// in "action".
static bool isRoleWord(const std::string& v) {
    std::string s = toLower(v);
    return s == "design" || s == "designer" || s == "code" || s == "coder" ||
           s == "programming" || s == "programmer" || s == "gameplay";
}

// "new_zone" / "plan-1" -> "New Zone" / "Plan 1" (title fallback from a JSON key).
static std::string prettifyKey(std::string s) {
    for (char& c : s)
        if (c == '_' || c == '-') c = ' ';
    bool cap = true;
    for (char& c : s) {
        if (cap && c >= 'a' && c <= 'z') c = (char)(c - 32);
        cap = (c == ' ');
    }
    return s;
}

// The planner's JSON shape changes every run (2-3 plans-with-steps, a category
// tree, or — this is the important one — a FLAT list where each item is itself a
// single step). Instead of matching shapes, we normalize around one unit: the
// STEP. A plan is just a group of steps; when there is no grouping, all the
// steps ARE one plan. Everything below reads fields by alias and looks into a
// nested "details" object, because models scatter the real content there.

// Pulls file paths out of a step object (+ its nested details): single-string
// keys and string arrays alike.
static void collectStepFiles(const JsonValue& j, std::vector<std::string>& out) {
    for (const char* k : {"file", "file_path", "filePath", "path", "artifact", "output"})
        if (const std::string* s = j.string(k))
            if (!s->empty()) out.push_back(*s);
    for (const char* k : {"files", "file_paths", "artifacts", "paths", "outputs"})
        if (const JsonValue* a = j.find(k))
            if (a->type == JsonValue::Array)
                for (size_t i = 0; i < a->size(); ++i)
                    if ((*a)[i].type == JsonValue::String) out.push_back((*a)[i].str);
    for (const char* k : {"details", "detail", "info"})
        if (const JsonValue* d = j.find(k))
            if (d->type == JsonValue::Object) collectStepFiles(*d, out);
}

// Normalize any "step-ish" object to a PlanStep; appends its file paths to `files`.
static DevTeam::PlanStep extractStep(const JsonValue& sj, std::vector<std::string>& files) {
    const JsonValue* det = nullptr;
    for (const char* k : {"details", "detail", "info"})
        if (const JsonValue* d = sj.find(k))
            if (d->type == JsonValue::Object) { det = d; break; }

    DevTeam::PlanStep st;
    st.agent = jsonFirstString(sj, {"agent", "specialist", "role", "who"});
    std::string stepTxt = jsonFirstString(sj, {"step", "title", "name"});
    // "step":"design" — the role is in step, the real task is elsewhere.
    if (st.agent.empty() && isRoleWord(stepTxt)) { st.agent = stepTxt; stepTxt.clear(); }
    std::string action = jsonFirstString(sj, {"action", "task", "description", "what", "goal"});
    if (action.empty() && det)
        action = jsonFirstString(*det, {"task", "action", "description", "step", "title", "what"});
    if (stepTxt.empty() && det) stepTxt = jsonFirstString(*det, {"step", "title", "name"});
    st.title = !stepTxt.empty() ? stepTxt : action;
    if (st.title.empty()) st.title = jsonFirstString(sj, {"detail", "summary"});
    st.detail = (!action.empty() && action != st.title) ? action : "";
    st.agent = (toLower(st.agent) == "design" || toLower(st.agent) == "designer") ? "design" : "code";
    collectStepFiles(sj, files);
    return st;
}

// The first array of step-OBJECTS an object owns (steps/tasks/actions/...).
static const JsonValue* stepsArrayOf(const JsonValue& obj) {
    const JsonValue* a = jsonFirstArray(obj, {"steps", "tasks", "actions", "subtasks"});
    return (a && a->size() > 0 && (*a)[0].type == JsonValue::Object) ? a : nullptr;
}

// Pass 1: real plans — objects that own a steps array — however nested.
static void collectPlanNodes(const JsonValue& j, const std::string& hint,
                             std::vector<DevTeam::Plan>& out) {
    if (j.type == JsonValue::Array) {
        for (size_t i = 0; i < j.size(); ++i) collectPlanNodes(j[i], hint, out);
        return;
    }
    if (j.type != JsonValue::Object) return;
    if (const JsonValue* steps = stepsArrayOf(j)) {
        DevTeam::Plan p;
        p.title = jsonFirstString(j, {"title", "increment", "name", "approach"});
        if (p.title.empty() && !hint.empty()) p.title = prettifyKey(hint);
        p.summary = jsonFirstString(j, {"summary", "description", "overview"});
        for (size_t k = 0; k < steps->size(); ++k)
            p.steps.push_back(extractStep((*steps)[k], p.artifacts));
        if (const JsonValue* arts = jsonFirstArray(j, {"artifacts", "files", "outputs"}))
            for (size_t k = 0; k < arts->size(); ++k)
                if ((*arts)[k].type == JsonValue::String) p.artifacts.push_back((*arts)[k].str);
        if (p.title.empty()) p.title = "Plan " + std::to_string(out.size() + 1);
        if (!p.steps.empty()) out.push_back(std::move(p));
        return;
    }
    for (const auto& kv : j.obj) {
        std::string h = hint.empty() ? kv.first : hint + " " + kv.first;
        collectPlanNodes(kv.second, h, out);
    }
}

// A step-ish object carries descriptive text or a role (fallback pass only).
static bool looksLikeStep(const JsonValue& j) {
    return j.type == JsonValue::Object &&
           (!jsonFirstString(j, {"step", "action", "task", "title", "name", "description"}).empty() ||
            !jsonFirstString(j, {"specialist", "agent", "role"}).empty());
}

// Pass 2 (fallback): no steps arrays anywhere — the items themselves are steps.
static void collectLooseSteps(const JsonValue& j, DevTeam::Plan& plan) {
    if (j.type == JsonValue::Array) {
        for (size_t i = 0; i < j.size(); ++i) collectLooseSteps(j[i], plan);
        return;
    }
    if (j.type != JsonValue::Object) return;
    if (looksLikeStep(j)) { plan.steps.push_back(extractStep(j, plan.artifacts)); return; }
    for (const auto& kv : j.obj) collectLooseSteps(kv.second, plan);
}

// Turn any planner response into 1+ plans: prefer real grouped plans; if the
// model produced a bare list of steps, wrap them into a single plan.
static std::vector<DevTeam::Plan> normalizePlans(const JsonValue& parsed) {
    const JsonValue* root = parsed.find("plans");
    if (!root) root = parsed.find("increments");
    if (!root) root = parsed.find("plan");
    const JsonValue& r = root ? *root : parsed;

    std::vector<DevTeam::Plan> out;
    collectPlanNodes(r, "", out);
    if (out.empty()) {
        DevTeam::Plan single;
        single.title = "Proposed plan";
        collectLooseSteps(r, single);
        if (!single.steps.empty()) out.push_back(std::move(single));
    }
    return out;
}

// Same shape-tolerance for specialist output: a "file" is any object carrying
// both a path and content (under whatever key names / nesting the model chose).
struct RawFile {
    std::string path, kind, description, content;
};
static void collectFiles(const JsonValue& j, std::vector<RawFile>& out) {
    if (j.type == JsonValue::Array) {
        for (size_t i = 0; i < j.size(); ++i) collectFiles(j[i], out);
        return;
    }
    if (j.type != JsonValue::Object) return;
    std::string path =
        jsonFirstString(j, {"path", "file", "filename", "filePath", "relPath", "name"});
    std::string content = jsonFirstString(j, {"content", "code", "text", "body", "data", "source"});
    // Models often return a .json file's content as a real JSON OBJECT/ARRAY
    // (not a string) — serialize it back to text so the proposal is usable.
    if (content.empty())
        for (const char* k : {"content", "data", "body", "json", "file"})
            if (const JsonValue* cv = j.find(k))
                if (cv->type == JsonValue::Object || cv->type == JsonValue::Array) {
                    content = jsonToText(*cv);
                    break;
                }
    if (!path.empty() && !content.empty()) {
        RawFile f;
        f.path = path;
        f.content = content;
        f.kind = jsonFirstString(j, {"kind", "type", "category"});
        f.description = jsonFirstString(j, {"description", "detail", "summary", "purpose"});
        out.push_back(std::move(f));
        return;
    }
    for (const auto& kv : j.obj) collectFiles(kv.second, out);
}

// Grounding: include a few REAL project files verbatim so the specialist copies
// the engine's ACTUAL formats. Local models follow a concrete example far better
// than a prose schema — this is the single biggest lever on output quality. Uses
// the smallest .json in each asset dir (the cleanest minimal example).
static std::string assetExamples(const std::string& root) {
    struct Pick { const char* dir; const char* label; };
    const Pick picks[] = {{"assets/maps", "SCENE (assets/maps/*.json)"},
                          {"assets/scripts", "SCRIPT GRAPH (assets/scripts/*.json)"},
                          {"assets/data", "DATA TABLE (assets/data/*.json)"},
                          {"assets/ui", "UI DOCUMENT (assets/ui/*.json)"}};
    std::ostringstream o;
    for (const Pick& p : picks) {
        std::string best;
        uintmax_t bestSize = UINTMAX_MAX;
        std::error_code ec;
        for (fs::directory_iterator it(joinPath(root, p.dir), ec), end; it != end && !ec;
             it.increment(ec)) {
            if (it->is_directory() || it->path().extension() != ".json") continue;
            uintmax_t sz = fs::file_size(it->path(), ec);
            if (!ec && sz < bestSize) { bestSize = sz; best = it->path().string(); }
        }
        if (best.empty()) continue;
        std::string content = readTextFile(best);
        if (content.size() > 2500) content = content.substr(0, 2500) + "\n...(truncated)";
        o << "=== " << p.label << " — copy this exact structure/keys ===\n" << content << "\n\n";
    }
    return o.str();
}

// Validation: check a proposed file against the engine's real format so broken
// output is caught BEFORE it's written (and can be fed back to the specialist).
// Returns "" when fine, else a semicolon list of problems.
static std::string validateProposal(const std::string& path, const std::string& content) {
    bool isJson = path.size() > 5 && toLower(path.substr(path.size() - 5)) == ".json";
    if (!isJson) return {};
    JsonValue doc;
    if (!jsonParse(content.c_str(), content.size(), doc)) return "not valid JSON";

    std::ostringstream out;
    auto add = [&](const std::string& s) { out << (out.tellp() > 0 ? "; " : "") << s; };
    bool haveRegistry = !componentRegistry().all().empty();

    // A scene is recognizable by an "entities" array — validate its structure
    // against the loader's expectations (the #1 source of "applied but broken").
    if (const JsonValue* ents = doc.find("entities")) {
        if (!doc.find("version")) add("scene is missing top-level \"version\": 1");
        for (size_t i = 0; i < ents->size(); ++i) {
            const JsonValue& e = (*ents)[i];
            std::string nm = e.string("name") ? *e.string("name") : ("entity[" + std::to_string(i) + "]");
            if (!e.find("position") || !e.find("rotation") || !e.find("scale"))
                add(nm + ": needs position[3]+rotation[4]+scale[3]");
            const JsonValue* comps = e.find("components");
            if (!comps) { add(nm + ": no \"components\" array"); continue; }
            for (size_t c = 0; c < comps->size(); ++c) {
                const std::string* t = (*comps)[c].string("type");
                if (!t || t->empty()) { add(nm + ": a component has no \"type\""); continue; }
                if (haveRegistry && !componentRegistry().find(*t))
                    add(nm + ": unknown component \"" + *t + "\"");
            }
        }
    }
    return out.str();
}

// ---------------------------------------------------------------------------
// personas
// ---------------------------------------------------------------------------
namespace {

struct PersonaSpec {
    const char* role;
    const char* name;
    const char* apiRole;
    const char* personality;
    const char* mission;
    const char* style;
    const char* systemPrompt;
};

const PersonaSpec kPersonas[] = {
    {"planner", "Aether Planner", "Technical game producer",
     "Structured, pragmatic, allergic to scope creep. Thinks in shippable increments.",
     "Turn a game developer's one-line request into small, complete, alternative "
     "implementation plans for the Aether Engine, choosing the right specialist for "
     "each step.",
     "Options-first: always propose genuinely different approaches with tradeoffs.",
     "You are the planner of a small AI dev team embedded in Aether Engine (a "
     "JSON-everything game engine: scenes, visual scripts, data tables, UI documents "
     "are all plain JSON files; native C++ scripts also exist). You break requests "
     "into steps assigned to the 'code' specialist (script graphs, data tables, C++) "
     "or the 'design' specialist (scenes, UI documents, tuning). Plans must be small, "
     "complete, and use the engine's real capabilities as documented in your "
     "knowledge sources."},
    {"code", "Aether Coder", "Gameplay programmer",
     "Precise, minimal, verification-minded. Writes exactly what the format reference says.",
     "Author complete, valid Aether Engine artifacts: visual script graphs, data "
     "tables, and C++ script components, exactly following the project's format "
     "reference.",
     "Correctness over cleverness; small files over monoliths.",
     "You write Aether Engine game content as COMPLETE files. Script graphs are JSON "
     "(scriptGraph: 2) with nodes/exec/in links exactly as the node reference "
     "documents. Data tables are JSON with columns + rows. C++ scripts derive from "
     "ae::Behavior with AE_COMPONENT + reflect(). Never invent node types or "
     "component fields - use only what the documentation in your knowledge sources "
     "defines. Every file you emit must be complete and valid."},
    {"design", "Aether Designer", "Technical game designer",
     "Player-focused, concrete, economical. Designs with the engine's real building blocks.",
     "Author scenes, UI documents, and tuning values for Aether Engine projects, and "
     "describe layout intent the developer can refine in the editor.",
     "Show, don't tell: produce placeable content, not essays.",
     "You design Aether Engine content as COMPLETE files. Scenes/maps are JSON entity "
     "arrays (name/guid/parent/position/rotation/scale/components) exactly as the "
     "scene format documents; components must match the component reference. UI "
     "documents and data tables are JSON per the docs in your knowledge sources. "
     "Invent nothing outside the documented formats; every file must be complete."},
};

std::string readTextFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// JsonValue -> compact text (the parser has no writer; the bridge wants the
// personas' `params` objects back as JSON).
std::string jsonToText(const JsonValue& v) {
    switch (v.type) {
    case JsonValue::Null: return "null";
    case JsonValue::Bool: return v.boolean ? "true" : "false";
    case JsonValue::Number: {
        char b[48];
        std::snprintf(b, sizeof(b), "%g", v.number);
        return b;
    }
    case JsonValue::String: return "\"" + pulseJsonEscape(v.str) + "\"";
    case JsonValue::Array: {
        std::string out = "[";
        for (size_t i = 0; i < v.arr.size(); ++i)
            out += (i ? "," : "") + jsonToText(v.arr[i]);
        return out + "]";
    }
    case JsonValue::Object: {
        std::string out = "{";
        bool first = true;
        for (const auto& kv : v.obj) {
            out += std::string(first ? "" : ",") + "\"" + pulseJsonEscape(kv.first) +
                   "\":" + jsonToText(kv.second);
            first = false;
        }
        return out + "}";
    }
    }
    return "null";
}

// The shared response contract for both specialists (generate + refine).
const char* kSpecialistSchema =
    "{ \"files\": [ { \"path\": \"project-relative path like "
    "assets/scripts/name.json\", \"kind\": \"script-graph | data-table | "
    "ui-document | scene | cpp | other\", \"description\": \"one line\", "
    "\"content\": \"the COMPLETE file content as a string\" } ], "
    "\"bridgeCalls\": [ { \"method\": \"agent-bridge method like entity.spawn "
    "(see Docs/reference/agent-bridge.md)\", \"params\": { } } ], "
    "\"notes\": \"anything the developer must do manually\" } - bridgeCalls is "
    "OPTIONAL: live-editor steps (spawn/wire/verify) the developer can run "
    "after applying your files";

// FNV-1a 64 as hex — cheap change detection for the knowledge sync.
std::string contentHash(const std::string& text) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : text) {
        h ^= c;
        h *= 1099511628211ull;
    }
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return buf;
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle + snapshots
// ---------------------------------------------------------------------------

DevTeam::~DevTeam() { joinWorker(); }

void DevTeam::joinWorker() {
    if (worker_.joinable()) worker_.join();
}

void DevTeam::init(PulseClient* client, const std::string& projectRoot) {
    client_ = client;
    projectRoot_ = projectRoot;
    std::string dir = joinPath(projectRoot, "Intermediate\\AiAssist");
    CreateDirectoryA(joinPath(projectRoot, "Intermediate").c_str(), nullptr);
    CreateDirectoryA(dir.c_str(), nullptr);
    stateFile_ = joinPath(dir, "state.json");

    agents_.clear();
    for (const PersonaSpec& p : kPersonas) {
        AgentSlot s;
        s.role = p.role;
        s.title = p.name;
        agents_.push_back(std::move(s));
    }
    loadState();
}

std::vector<DevTeam::StageInfo> DevTeam::stages() const {
    std::lock_guard<std::mutex> l(mtx_);
    return std::vector<StageInfo>(stages_, stages_ + StageCount);
}
std::vector<DevTeam::AgentSlot> DevTeam::agents() const {
    std::lock_guard<std::mutex> l(mtx_);
    return agents_;
}
std::vector<DevTeam::Plan> DevTeam::plans() const {
    std::lock_guard<std::mutex> l(mtx_);
    return plans_;
}
std::vector<DevTeam::Proposal> DevTeam::proposals() const {
    std::lock_guard<std::mutex> l(mtx_);
    return proposals_;
}
std::vector<std::string> DevTeam::log() const {
    std::lock_guard<std::mutex> l(mtx_);
    return log_;
}

void DevTeam::setStage(Stage s, State st, const std::string& detail) {
    std::lock_guard<std::mutex> l(mtx_);
    stages_[s].state = st;
    stages_[s].detail = detail;
}
void DevTeam::setAgent(const std::string& role, State st) {
    std::lock_guard<std::mutex> l(mtx_);
    for (AgentSlot& a : agents_)
        if (a.role == role) a.state = st;
}
void DevTeam::addLog(const std::string& line) {
    std::lock_guard<std::mutex> l(mtx_);
    log_.push_back(line);
}
DevTeam::AgentSlot* DevTeam::slot(const std::string& role) {
    for (AgentSlot& a : agents_)
        if (a.role == role) return &a;
    return nullptr;
}

// ---------------------------------------------------------------------------
// persisted assistant state (agent ids, knowledge flag)
// ---------------------------------------------------------------------------

void DevTeam::loadState() {
    std::string text = readTextFile(stateFile_);
    if (text.empty()) return;
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc)) return;
    for (AgentSlot& a : agents_)
        if (const JsonValue* agents = doc.find("agents"))
            if (const std::string* id = agents->string(a.role.c_str())) a.id = *id;
    // Per-file ingestion record. (Older state.json had a single knowledgeReady
    // flag; ignoring it just re-syncs every doc once.)
    if (const JsonValue* k = doc.find("knowledge"))
        for (const auto& kv : k->obj)
            if (kv.second.type == JsonValue::String)
                knowledgeHashes_[kv.first] = kv.second.str;
}

void DevTeam::saveState() {
    std::ofstream f(stateFile_, std::ios::binary);
    if (!f) return;
    f << "{\n  \"agents\": {";
    bool first = true;
    for (const AgentSlot& a : agents_) {
        if (a.id.empty()) continue;
        f << (first ? " " : ", ") << "\"" << a.role << "\": \"" << a.id << "\"";
        first = false;
    }
    f << " },\n  \"knowledge\": {";
    first = true;
    for (const auto& kv : knowledgeHashes_) {
        f << (first ? "\n    " : ",\n    ") << "\"" << kv.first << "\": \"" << kv.second << "\"";
        first = false;
    }
    f << (first ? "" : "\n  ") << "}\n}\n";
}

// ---------------------------------------------------------------------------
// worker stages
// ---------------------------------------------------------------------------

bool DevTeam::ensureTeam() {
    setStage(StageTeam, State::Running);
    std::string err;
    std::vector<PulseAgent> existing = client_->listAgents(&err);
    if (!err.empty()) {
        setStage(StageTeam, State::Error, err);
        return false;
    }
    for (const PersonaSpec& p : kPersonas) {
        AgentSlot* s;
        {
            std::lock_guard<std::mutex> l(mtx_);
            s = slot(p.role);
        }
        if (!s->id.empty()) continue;
        for (const PulseAgent& a : existing)
            if (a.name == p.name) s->id = a.id;
        if (!s->id.empty()) {
            addLog(std::string("found existing persona: ") + p.name);
            continue;
        }
        std::string id = client_->createAgent(p.name, p.apiRole, p.personality, p.mission,
                                              p.style, p.systemPrompt, &err);
        if (id.empty()) {
            setStage(StageTeam, State::Error, err);
            return false;
        }
        {
            std::lock_guard<std::mutex> l(mtx_);
            slot(p.role)->id = id;
        }
        addLog(std::string("created persona: ") + p.name);
    }
    {
        std::lock_guard<std::mutex> l(mtx_);
        saveState();
    }
    setStage(StageTeam, State::Done, "3 personas ready");
    return true;
}

bool DevTeam::ensureKnowledge() {
    setStage(StageKnowledge, State::Running, "checking Docs/ against the knowledge base");
    std::vector<std::string> agentIds;
    {
        std::lock_guard<std::mutex> l(mtx_);
        for (const AgentSlot& a : agents_) agentIds.push_back(a.id);
    }

    // Every markdown file under Docs/ is ground truth (format guides + the
    // generated reference). Diff against the per-file record so docs added or
    // regenerated since the last run are picked up instead of silently missing.
    struct DocFile {
        std::string rel, text, hash;
    };
    std::vector<DocFile> stale;
    int current = 0;
    std::error_code ec;
    fs::path root(projectRoot_);
    for (fs::recursive_directory_iterator it(root / "Docs", ec), end; it != end && !ec;
         it.increment(ec)) {
        if (it->is_directory() || it->path().extension() != ".md") continue;
        std::string text = readTextFile(it->path().string());
        if (text.empty()) continue;
        DocFile d{fs::relative(it->path(), root, ec).generic_string(), std::move(text), ""};
        d.hash = contentHash(d.text);
        bool known;
        {
            std::lock_guard<std::mutex> l(mtx_);
            auto f = knowledgeHashes_.find(d.rel);
            known = f != knowledgeHashes_.end() && f->second == d.hash;
        }
        if (known) ++current;
        else stale.push_back(std::move(d));
    }
    if (stale.empty()) {
        setStage(StageKnowledge, State::Done,
                 std::to_string(current) + " docs current — nothing missing");
        return true;
    }

    addLog(std::to_string(stale.size()) + " doc(s) new or changed — ingesting");
    setStage(StageKnowledge, State::Running,
             "ingesting " + std::to_string(stale.size()) + " doc(s) into Pulse Cortex");
    for (const DocFile& d : stale) {
        std::string err;
        // Stable source name per file, so the backend can upsert rather than
        // accumulate versions.
        std::string id = client_->ingestKnowledge("Aether: " + d.rel, d.text, agentIds, &err);
        if (id.empty()) {
            setStage(StageKnowledge, State::Error, err);
            return false;
        }
        addLog("ingested " + d.rel);
        {
            std::lock_guard<std::mutex> l(mtx_);
            knowledgeHashes_[d.rel] = d.hash;
            saveState(); // per file: an aborted run doesn't re-ingest what landed
        }
    }
    setStage(StageKnowledge, State::Done,
             std::to_string(stale.size()) + " ingested, " +
                 std::to_string(current + (int)stale.size()) + " docs total");
    return true;
}

// Per-call context so the personas know the live-editor bridge exists (their
// system prompts are persisted server-side; context blocks always reflect the
// current engine). Docs/reference/agent-bridge.md is in their RAG knowledge.
static PulseContextBlock bridgeContext(const PulseClient* client) {
    int port = client ? client->config.bridgePort : 3052;
    return {"engine", "Live editor bridge",
            "While the Aether editor is open, a control bridge runs at "
            "http://127.0.0.1:" + std::to_string(port) + " (PulseLABS-gated: "
            "x-aether-token from pulse.json; GET /health probes it; POST /rpc "
            "{\"method\",\"params\"}). It can spawn/modify entities, set components, "
            "start/stop Play, read logs, and take viewport screenshots — see the "
            "'Aether: Docs/reference/agent-bridge.md' knowledge source for every "
            "method. When a plan needs in-editor verification or scene wiring, "
            "include a step that says exactly which bridge calls to make."};
}

static std::string projectInventory(const std::string& root) {
    std::ostringstream o;
    const char* dirs[] = {"assets/maps", "assets/scripts", "assets/data", "assets/ui"};
    for (const char* d : dirs) {
        o << d << ":";
        std::error_code ec;
        bool any = false;
        for (fs::directory_iterator it(joinPath(root, d), ec), end; it != end && !ec;
             it.increment(ec)) {
            if (it->is_directory()) continue;
            o << " " << it->path().filename().string();
            any = true;
        }
        if (!any) o << " (none)";
        o << "\n";
    }
    return o.str();
}

void DevTeam::requestPlans(const std::string& prompt) {
    if (running_ || !client_) return;
    joinWorker();
    running_ = true;
    {
        std::lock_guard<std::mutex> l(mtx_);
        plans_.clear();
        proposals_.clear();
        log_.clear();
        selectedPlan_ = -1;
        for (int i = 0; i < StageCount; ++i) stages_[i] = {};
        stages_[StagePrompt] = {State::Done, prompt.substr(0, 60)};
        for (AgentSlot& a : agents_) {
            a.state = State::Idle;
            a.thoughts.clear();
            a.citations.clear();
        }
    }
    worker_ = std::thread([this, prompt]() {
        planJob(prompt);
        running_ = false;
    });
}

void DevTeam::planJob(std::string prompt) {
    if (!ensureTeam()) return;
    if (!ensureKnowledge()) return;

    setStage(StagePlan, State::Running, "planner drafting alternatives");
    setAgent("planner", State::Running);

    std::string plannerId;
    {
        std::lock_guard<std::mutex> l(mtx_);
        plannerId = slot("planner")->id;
    }

    PulseClient::ChatOptions opt;
    opt.jsonSchema =
        "{ \"plans\": [ { \"title\": \"short name of the approach\", \"summary\": "
        "\"2-3 sentences: what gets built and why this approach\", \"steps\": [ { "
        "\"title\": \"what to do, as a full sentence\", \"agent\": \"code\" or "
        "\"design\", \"file\": \"project-relative path this step creates (optional)\" "
        "} ] } ] } . Rules: 'plans' is a flat ARRAY of 2-3 alternative complete "
        "plans. Each plan has a 'steps' ARRAY. Put the human-readable instruction "
        "in the step's 'title'; put the role in 'agent'. Do NOT nest the step text "
        "inside a 'details' object. Use these exact field names.";
    opt.knowledge = true;
    opt.reflection = true;
    opt.maxTokens = 3500;
    opt.context.push_back({"project", "Existing project files", projectInventory(projectRoot_)});
    opt.context.push_back(
        {"engine", "Engine facts",
         "Aether Engine: scenes/scripts/data/UI are JSON files under assets/. Gameplay "
         "logic should prefer visual script graphs (assets/scripts/*.json) attached via "
         "a ScriptGraph component. New file paths must not collide with existing ones."});
    opt.context.push_back(bridgeContext(client_));

    PulseChatResult r = client_->chat(plannerId, prompt, opt);
    // Always capture the raw response — the panel's "raw" button shows it, which
    // is the fastest way to see WHY a model's output failed to parse.
    {
        std::lock_guard<std::mutex> l(mtx_);
        AgentSlot* s = slot("planner");
        s->lastRaw = r.content;
        s->thoughts = r.thoughts;
        s->citations = r.knowledgeUsed;
        if (!r.sessionId.empty()) plannerSession_ = r.sessionId;
    }
    if (!r.ok) {
        setAgent("planner", State::Error);
        setStage(StagePlan, State::Error, r.error);
        return;
    }
    if (!r.hasParsed) {
        setAgent("planner", State::Error);
        setStage(StagePlan, State::Error,
                 "planner did not return JSON — click 'raw' on the Planner to see its output");
        return;
    }
    { std::lock_guard<std::mutex> l(mtx_); slot("planner")->state = State::Done; }

    // normalizePlans handles every shape seen from local models: 2-3 plans each
    // with steps, a category tree, or a flat list of bare steps (→ one plan).
    std::vector<Plan> parsed = normalizePlans(r.parsed);
    if (parsed.empty()) {
        setAgent("planner", State::Error);
        setStage(StagePlan, State::Error,
                 "planner JSON had no usable plans — click 'raw' on the Planner to inspect");
        return;
    }
    {
        std::lock_guard<std::mutex> l(mtx_);
        plans_ = std::move(parsed);
    }
    addLog("planner proposed " + std::to_string(plans().size()) + " alternative plans");
    setStage(StagePlan, State::Done, std::to_string(plans().size()) + " alternatives - pick one");
}

void DevTeam::generate(int planIndex) {
    if (running_ || !client_) return;
    {
        std::lock_guard<std::mutex> l(mtx_);
        if (planIndex < 0 || planIndex >= (int)plans_.size()) return;
        selectedPlan_ = planIndex;
        proposals_.clear();
    }
    joinWorker();
    running_ = true;
    worker_ = std::thread([this, planIndex]() {
        generateJob(planIndex);
        running_ = false;
    });
}

// One specialist chat turn — used by the first generation AND by refine():
// refine passes the same role with a sessionId already on the slot, so the
// persona revises inside the same conversation. Files merge by path (a
// revised file replaces the old proposal and goes back to un-applied);
// requested bridgeCalls queue up for the user to run.
bool DevTeam::specialistTurn(const std::string& role, const std::string& message) {
    setAgent(role, State::Running);
    std::string agentId, sessionId;
    {
        std::lock_guard<std::mutex> l(mtx_);
        agentId = slot(role)->id;
        sessionId = slot(role)->sessionId;
    }

    PulseClient::ChatOptions opt;
    opt.sessionId = sessionId;
    opt.jsonSchema = kSpecialistSchema;
    opt.knowledge = true;
    opt.knowledgeTopK = 8;
    opt.reflection = true;
    opt.maxTokens = 4000;
    opt.temperature = 0.3f;
    opt.context.push_back({"project", "Existing project files", projectInventory(projectRoot_)});
    if (std::string ex = assetExamples(projectRoot_); !ex.empty())
        opt.context.push_back({"examples", "Real project files — match these formats EXACTLY", ex});
    opt.context.push_back(bridgeContext(client_));

    PulseChatResult r = client_->chat(agentId, message, opt);
    {
        std::lock_guard<std::mutex> l(mtx_);
        AgentSlot* s = slot(role);
        s->lastRaw = r.content; // keep raw output for the panel's "raw" viewer
        s->thoughts = r.thoughts;
        s->citations = r.knowledgeUsed;
        if (!r.sessionId.empty()) s->sessionId = r.sessionId;
    }
    if (!r.ok) {
        setAgent(role, State::Error);
        setStage(StageGenerate, State::Error, role + ": " + r.error);
        return false;
    }
    if (!r.hasParsed) {
        setAgent(role, State::Error);
        setStage(StageGenerate, State::Error,
                 role + " did not return JSON — click 'raw' on the agent to inspect");
        return false;
    }
    { std::lock_guard<std::mutex> l(mtx_); slot(role)->state = State::Done; }

    // Collect proposed files wherever/however the model nested them.
    std::vector<RawFile> rawFiles;
    const JsonValue* filesNode = jsonFirstArray(r.parsed, {"files", "artifacts", "outputs"});
    collectFiles(filesNode ? *filesNode : r.parsed, rawFiles);
    for (const RawFile& rf : rawFiles) {
        Proposal p;
        p.path = rf.path;
        p.kind = rf.kind;
        p.description = rf.description;
        p.content = rf.content;
        p.agent = role;
        // If the model gave a bare filename (no folder), place it in the
        // conventional directory for its kind so it doesn't land at the project
        // root. Heuristic, and the user reviews every proposal before applying.
        if (p.path.find('/') == std::string::npos && p.path.find('\\') == std::string::npos) {
            std::string k = toLower(p.kind);
            auto ends = [&](const char* e) {
                size_t n = std::strlen(e);
                return p.path.size() >= n && toLower(p.path.substr(p.path.size() - n)) == e;
            };
            std::string dir;
            if (ends(".cpp") || ends(".h") || ends(".hpp") || k == "cpp" || k == "c++" ||
                k == "source" || k == "native")
                dir = "Source/";
            else if (k == "map" || k == "scene" || k == "level") dir = "assets/maps/";
            else if (k == "script" || k == "graph" || k == "blueprint") dir = "assets/scripts/";
            else if (k == "data" || k == "datatable" || k == "table") dir = "assets/data/";
            else if (k == "ui" || k == "uidocument" || k == "hud") dir = "assets/ui/";
            else if (k == "material") dir = "assets/materials/";
            else if (k == "dialogue") dir = "assets/dialogue/";
            else if (k == "mission" || k == "missions") dir = "assets/missions/";
            if (!dir.empty()) p.path = dir + p.path;
        }
        p.exists = pathExists(joinPath(projectRoot_, p.path));
        if (p.path.size() > 5 && p.path.substr(p.path.size() - 5) == ".json") {
            JsonValue chk;
            p.jsonValid = jsonParse(p.content.c_str(), p.content.size(), chk);
        }
        // Validate against the engine's real format (structure + component types).
        p.issues = validateProposal(p.path, p.content);
        std::lock_guard<std::mutex> l(mtx_);
        bool replaced = false;
        for (Proposal& old : proposals_)
            if (old.path == p.path) {
                old = p; // revision: back to un-applied, re-reviewed
                replaced = true;
                break;
            }
        if (!replaced) proposals_.push_back(p);
        log_.push_back(role + (replaced ? " revised " : " proposed ") + p.path +
                       (!p.jsonValid ? " (INVALID JSON)" : p.issues.empty() ? "" : " (format issues)"));
    }
    const JsonValue* calls = jsonFirstArray(r.parsed, {"bridgeCalls", "editorCalls", "calls"});
    if (calls) {
        for (size_t i = 0; i < calls->size(); ++i) {
            const JsonValue& cj = (*calls)[i];
            std::string method = jsonFirstString(cj, {"method", "name", "action"});
            if (method.empty()) continue;
            BridgeCall bc;
            bc.method = method;
            const JsonValue* params = cj.find("params");
            if (!params) params = cj.find("arguments");
            if (!params) params = cj.find("args");
            bc.params = params ? jsonToText(*params) : "{}";
            bc.agent = role;
            addLog(role + " requests live-editor call: " + bc.method);
            std::lock_guard<std::mutex> l(mtx_);
            bridgeCalls_.push_back(std::move(bc));
        }
    }
    if (const std::string* notes = r.parsed.string("notes"))
        if (!notes->empty()) addLog(role + " notes: " + *notes);
    return true;
}

void DevTeam::generateJob(int planIndex) {
    Plan plan;
    {
        std::lock_guard<std::mutex> l(mtx_);
        plan = plans_[planIndex];
        bridgeCalls_.clear();
    }
    setStage(StageGenerate, State::Running, "specialists working");

    const char* roles[] = {"code", "design"};
    for (const char* role : roles) {
        std::ostringstream brief;
        int mine = 0;
        for (const PlanStep& s : plan.steps)
            if (s.agent == role) {
                brief << "- " << s.title << ": " << s.detail << "\n";
                ++mine;
            }
        if (mine == 0) continue;

        std::ostringstream msg;
        msg << "Plan: " << plan.title << " - " << plan.summary
            << "\nYour steps:\n" << brief.str()
            << "\nProduce every file your steps require, complete and valid.";
        if (!specialistTurn(role, msg.str())) return;
    }

    size_t n = proposals().size();
    setStage(StageGenerate, State::Done, std::to_string(n) + " files proposed");
    setStage(StageReview, n ? State::Running : State::Error,
             n ? "review + apply below" : "nothing proposed");
}

void DevTeam::refine(const std::string& feedback) {
    if (running_ || !client_ || feedback.empty()) return;
    joinWorker();
    running_ = true;
    worker_ = std::thread([this, feedback]() {
        refineJob(feedback);
        running_ = false;
    });
}

void DevTeam::refineJob(std::string feedback) {
    setStage(StageGenerate, State::Running, "revising with your feedback");
    addLog("feedback: " + feedback.substr(0, 80));

    // Only specialists that actually produced something get the feedback, in
    // their existing session so they see their own previous files.
    const char* roles[] = {"code", "design"};
    for (const char* role : roles) {
        bool hasWork = false;
        {
            std::lock_guard<std::mutex> l(mtx_);
            for (const Proposal& p : proposals_) hasWork |= p.agent == role;
            hasWork = hasWork && !slot(role)->sessionId.empty();
        }
        if (!hasWork) continue;
        std::string msg =
            "Developer feedback on the files you proposed:\n" + feedback +
            "\nRevise accordingly. Return ONLY the files that change (or are new), "
            "each COMPLETE and valid — same JSON format as before.";
        if (!specialistTurn(role, msg)) return;
    }

    size_t n = proposals().size();
    setStage(StageGenerate, State::Done, std::to_string(n) + " files after revision");
    setStage(StageReview, n ? State::Running : State::Error,
             n ? "review + apply below" : "nothing proposed");
}

// ---------------------------------------------------------------------------
// agent-bridge execution (worker thread — the bridge is serviced by the
// editor MAIN thread each frame, so calling it from a worker cannot deadlock)
// ---------------------------------------------------------------------------

std::vector<DevTeam::BridgeCall> DevTeam::bridgeCalls() const {
    std::lock_guard<std::mutex> l(mtx_);
    return bridgeCalls_;
}

void DevTeam::runBridgeCalls() {
    if (running_ || !client_) return;
    joinWorker();
    running_ = true;
    worker_ = std::thread([this]() {
        bridgeCallJob();
        running_ = false;
    });
}

void DevTeam::bridgeCallJob() {
    const PulseConfig& cfg = client_->config;
    std::string base = "http://127.0.0.1:" + std::to_string(cfg.bridgePort);
    std::vector<HttpHeader> auth = {{"x-aether-token", cfg.bridgeToken}};

    std::vector<BridgeCall> pending;
    {
        std::lock_guard<std::mutex> l(mtx_);
        for (const BridgeCall& c : bridgeCalls_)
            if (!c.ran) pending.push_back(c);
    }
    for (BridgeCall& c : pending) {
        std::string body = "{\"method\":\"" + pulseJsonEscape(c.method) +
                           "\",\"params\":" + (c.params.empty() ? "{}" : c.params) + "}";
        HttpResponse r = httpPost(base + "/rpc", body, auth, 30000);
        c.ran = true;
        c.ok = r.ok() && r.body.find("\"ok\":true") != std::string::npos;
        c.result = r.status == 0 ? "bridge unreachable: " + r.error : r.body;
        addLog("bridge " + c.method + (c.ok ? " ok" : " FAILED"));
        std::lock_guard<std::mutex> l(mtx_);
        for (BridgeCall& stored : bridgeCalls_)
            if (!stored.ran && stored.method == c.method && stored.params == c.params) {
                stored = c;
                break;
            }
    }
}

bool DevTeam::applyProposal(size_t index, std::string* error) {
    Proposal p;
    {
        std::lock_guard<std::mutex> l(mtx_);
        if (index >= proposals_.size()) return false;
        p = proposals_[index];
    }
    if (!p.jsonValid) {
        if (error) *error = "refusing to write invalid JSON";
        return false;
    }
    std::string full = joinPath(projectRoot_, p.path);
    std::error_code ec;
    fs::create_directories(fs::path(full).parent_path(), ec);
    std::ofstream f(full, std::ios::binary);
    if (!f) {
        if (error) *error = "cannot write " + full;
        return false;
    }
    f << p.content;
    if (!f.good()) {
        if (error) *error = "write failed: " + full;
        return false;
    }
    {
        std::lock_guard<std::mutex> l(mtx_);
        proposals_[index].applied = true;
        bool all = true;
        for (const Proposal& q : proposals_) all &= q.applied;
        if (all) stages_[StageReview] = {State::Done, "all proposals applied"};
    }
    AE_LOG("[AI] applied %s", p.path.c_str());
    // Proposed native code goes straight to a compiling, hot-reloaded DLL —
    // the editor hooks its script build here (see AiPanel/Editor wiring).
    if (p.kind == "cpp" && onCppApplied) onCppApplied(p.path);
    return true;
}

} // namespace ae
