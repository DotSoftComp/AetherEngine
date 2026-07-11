#include "dev_team.h"
#include "../core/log.h"
#include "../core/paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

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
    knowledgeReady_ = doc.flag("knowledgeReady", false);
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
    f << " },\n  \"knowledgeReady\": " << (knowledgeReady_ ? "true" : "false") << "\n}\n";
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
    if (knowledgeReady_) {
        setStage(StageKnowledge, State::Done, "docs already ingested");
        return true;
    }
    setStage(StageKnowledge, State::Running, "ingesting Docs/ into Pulse Cortex");
    std::vector<std::string> agentIds;
    {
        std::lock_guard<std::mutex> l(mtx_);
        for (const AgentSlot& a : agents_) agentIds.push_back(a.id);
    }
    // The generated reference + the format guides = the agents' ground truth.
    const char* docs[] = {"Docs/reference/script-nodes.md", "Docs/reference/components.md",
                          "Docs/script-graphs.md", "Docs/scenes-and-assets.md",
                          "Docs/scripting-cpp.md"};
    int ingested = 0;
    for (const char* rel : docs) {
        std::string text = readTextFile(joinPath(projectRoot_, rel));
        if (text.empty()) continue;
        std::string err;
        std::string id = client_->ingestKnowledge(std::string("Aether: ") + rel, text,
                                                  agentIds, &err);
        if (id.empty()) {
            setStage(StageKnowledge, State::Error, err);
            return false;
        }
        ++ingested;
        addLog(std::string("ingested ") + rel);
    }
    {
        std::lock_guard<std::mutex> l(mtx_);
        knowledgeReady_ = true;
        saveState();
    }
    setStage(StageKnowledge, State::Done,
             std::to_string(ingested) + " docs in the knowledge base");
    return true;
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
        "\"title\": \"step\", \"detail\": \"what exactly\", \"agent\": \"code or "
        "design\" } ], \"artifacts\": [ \"project-relative file paths that will be "
        "created\" ] } ] } - return exactly 2 or 3 genuinely different complete plans";
    opt.knowledge = true;
    opt.reflection = true;
    opt.maxTokens = 3500;
    opt.context.push_back({"project", "Existing project files", projectInventory(projectRoot_)});
    opt.context.push_back(
        {"engine", "Engine facts",
         "Aether Engine: scenes/scripts/data/UI are JSON files under assets/. Gameplay "
         "logic should prefer visual script graphs (assets/scripts/*.json) attached via "
         "a ScriptGraph component. New file paths must not collide with existing ones."});

    PulseChatResult r = client_->chat(plannerId, prompt, opt);
    if (!r.ok) {
        setAgent("planner", State::Error);
        setStage(StagePlan, State::Error, r.error);
        return;
    }
    {
        std::lock_guard<std::mutex> l(mtx_);
        AgentSlot* s = slot("planner");
        s->state = State::Done;
        s->thoughts = r.thoughts;
        s->citations = r.knowledgeUsed;
        plannerSession_ = r.sessionId;
    }
    if (!r.hasParsed) {
        setStage(StagePlan, State::Error, "planner returned no valid JSON");
        return;
    }

    std::vector<Plan> parsed;
    if (const JsonValue* plans = r.parsed.find("plans")) {
        for (size_t i = 0; i < plans->size(); ++i) {
            const JsonValue& pj = (*plans)[i];
            Plan p;
            if (const std::string* s = pj.string("title")) p.title = *s;
            if (const std::string* s = pj.string("summary")) p.summary = *s;
            if (const JsonValue* steps = pj.find("steps"))
                for (size_t k = 0; k < steps->size(); ++k) {
                    const JsonValue& sj = (*steps)[k];
                    PlanStep st;
                    if (const std::string* s = sj.string("title")) st.title = *s;
                    if (const std::string* s = sj.string("detail")) st.detail = *s;
                    if (const std::string* s = sj.string("agent")) st.agent = *s;
                    if (st.agent != "design") st.agent = "code";
                    p.steps.push_back(std::move(st));
                }
            if (const JsonValue* arts = pj.find("artifacts"))
                for (size_t k = 0; k < arts->size(); ++k)
                    if ((*arts)[k].type == JsonValue::String)
                        p.artifacts.push_back((*arts)[k].str);
            if (!p.title.empty()) parsed.push_back(std::move(p));
        }
    }
    if (parsed.empty()) {
        setStage(StagePlan, State::Error, "no plans in planner response");
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

void DevTeam::generateJob(int planIndex) {
    Plan plan;
    {
        std::lock_guard<std::mutex> l(mtx_);
        plan = plans_[planIndex];
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

        setAgent(role, State::Running);
        std::string agentId;
        {
            std::lock_guard<std::mutex> l(mtx_);
            agentId = slot(role)->id;
        }

        std::ostringstream msg;
        msg << "Plan: " << plan.title << " - " << plan.summary
            << "\nYour steps:\n" << brief.str()
            << "\nProduce every file your steps require, complete and valid.";

        PulseClient::ChatOptions opt;
        opt.jsonSchema =
            "{ \"files\": [ { \"path\": \"project-relative path like "
            "assets/scripts/name.json\", \"kind\": \"script-graph | data-table | "
            "ui-document | scene | cpp | other\", \"description\": \"one line\", "
            "\"content\": \"the COMPLETE file content as a string\" } ], \"notes\": "
            "\"anything the developer must do manually\" }";
        opt.knowledge = true;
        opt.knowledgeTopK = 8;
        opt.reflection = true;
        opt.maxTokens = 4000;
        opt.temperature = 0.3f;
        opt.context.push_back(
            {"project", "Existing project files", projectInventory(projectRoot_)});

        PulseChatResult r = client_->chat(agentId, msg.str(), opt);
        if (!r.ok) {
            setAgent(role, State::Error);
            setStage(StageGenerate, State::Error, std::string(role) + ": " + r.error);
            return;
        }
        {
            std::lock_guard<std::mutex> l(mtx_);
            AgentSlot* s = slot(role);
            s->state = State::Done;
            s->thoughts = r.thoughts;
            s->citations = r.knowledgeUsed;
        }
        if (!r.hasParsed) {
            setStage(StageGenerate, State::Error, std::string(role) + " returned no JSON");
            return;
        }
        if (const JsonValue* files = r.parsed.find("files")) {
            for (size_t i = 0; i < files->size(); ++i) {
                const JsonValue& fj = (*files)[i];
                Proposal p;
                if (const std::string* s = fj.string("path")) p.path = *s;
                if (const std::string* s = fj.string("kind")) p.kind = *s;
                if (const std::string* s = fj.string("description")) p.description = *s;
                if (const std::string* s = fj.string("content")) p.content = *s;
                p.agent = role;
                if (p.path.empty() || p.content.empty()) continue;
                p.exists = pathExists(joinPath(projectRoot_, p.path));
                if (p.path.size() > 5 && p.path.substr(p.path.size() - 5) == ".json") {
                    JsonValue chk;
                    p.jsonValid = jsonParse(p.content.c_str(), p.content.size(), chk);
                }
                addLog(std::string(role) + " proposed " + p.path +
                       (p.jsonValid ? "" : " (INVALID JSON)"));
                std::lock_guard<std::mutex> l(mtx_);
                proposals_.push_back(std::move(p));
            }
        }
        if (const std::string* notes = r.parsed.string("notes"))
            if (!notes->empty()) addLog(std::string(role) + " notes: " + *notes);
    }

    size_t n = proposals().size();
    setStage(StageGenerate, State::Done, std::to_string(n) + " files proposed");
    setStage(StageReview, n ? State::Running : State::Error,
             n ? "review + apply below" : "nothing proposed");
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
    return true;
}

} // namespace ae
