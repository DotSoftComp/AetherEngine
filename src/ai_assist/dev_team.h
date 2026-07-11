// Aether Engine — the AI dev team (PulseLABS-backed).
//
// One prompt in, reviewed artifacts out, through a visible pipeline:
//
//   prompt -> [team] -> [knowledge] -> [planner] -> [coder / designer] -> review
//
//   team       find-or-create the persistent personas on the PulseLABS side
//              (Planner / Gameplay Coder / Designer - real agents with memory)
//   knowledge  ingest the project's Docs/ (incl. the generated component +
//              script-node reference) into Pulse Cortex and link it to the
//              agents, so they answer grounded in THIS engine's actual API
//              (RAG - the citations are shown in the panel)
//   planner    turns the prompt into 2-3 ALTERNATIVE complete plans
//              (steps, which specialist does what, artifacts to create)
//   coder /    the chosen plan's steps fan out to the specialists, each
//   designer   producing complete project files (script graphs, data tables,
//              UI documents, scenes) as proposals
//   review     nothing touches the project until the user applies a proposal;
//              JSON artifacts are parse-checked before they can be applied
//
// All backend work runs on one worker thread; the editor panel polls the
// mutex-guarded snapshots. Nothing here blocks the frame.
#pragma once
#include "pulse_client.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ae {

class DevTeam {
public:
    enum Stage { StagePrompt, StageTeam, StageKnowledge, StagePlan, StageGenerate,
                 StageReview, StageCount };
    enum class State { Idle, Running, Done, Error };

    struct StageInfo {
        State state = State::Idle;
        std::string detail;
    };
    struct PlanStep {
        std::string title, detail, agent; // agent: "code" | "design"
    };
    struct Plan {
        std::string title, summary;
        std::vector<PlanStep> steps;
        std::vector<std::string> artifacts;
    };
    struct Proposal {
        std::string path;        // project-relative
        std::string kind;        // script-graph | data-table | ui-document | scene | cpp | other
        std::string description;
        std::string content;     // full file text
        std::string agent;       // which specialist produced it
        bool exists = false;     // would overwrite
        bool applied = false;
        bool jsonValid = true;   // parse-checked for .json artifacts
    };
    struct AgentSlot {
        std::string role;     // "planner" | "code" | "design"
        std::string title;    // display name
        std::string id;       // PulseLABS persona id
        State state = State::Idle;
        std::string thoughts; // last reflection trace
        std::vector<std::string> citations; // knowledge chunks used (RAG)
    };

    ~DevTeam();

    void init(PulseClient* client, const std::string& projectRoot);
    bool busy() const { return running_; }

    // Async: ensure team + knowledge, then ask the planner for alternatives.
    void requestPlans(const std::string& prompt);
    // Async: fan the chosen plan out to the specialists.
    void generate(int planIndex);

    // Main-thread snapshots (copies under lock).
    std::vector<StageInfo> stages() const;
    std::vector<AgentSlot> agents() const;
    std::vector<Plan> plans() const;
    std::vector<Proposal> proposals() const;
    std::vector<std::string> log() const;
    int selectedPlan() const { return selectedPlan_; }

    // Writes one proposal into the project (main thread; creates directories).
    bool applyProposal(size_t index, std::string* error);

private:
    void joinWorker();
    void setStage(Stage s, State st, const std::string& detail = "");
    void setAgent(const std::string& role, State st);
    void addLog(const std::string& line);
    bool ensureTeam();      // worker thread
    bool ensureKnowledge(); // worker thread
    void planJob(std::string prompt);
    void generateJob(int planIndex);
    AgentSlot* slot(const std::string& role);
    void loadState();
    void saveState();

    PulseClient* client_ = nullptr;
    std::string projectRoot_;
    std::string stateFile_;

    mutable std::mutex mtx_;
    StageInfo stages_[StageCount];
    std::vector<AgentSlot> agents_;
    std::vector<Plan> plans_;
    std::vector<Proposal> proposals_;
    std::vector<std::string> log_;
    int selectedPlan_ = -1;
    bool knowledgeReady_ = false;
    std::string plannerSession_;

    std::thread worker_;
    std::atomic<bool> running_{false};
};

} // namespace ae
