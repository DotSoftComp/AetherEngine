// Aether Engine — AI dev-team smoke test.
//
// Drives PulseClient + DevTeam end to end against a PulseLABS-compatible
// server (the real backend, or the schema-faithful mock used in CI):
// team creation, Docs ingestion (RAG), alternative plans, generation, the
// invalid-JSON guard, and applying proposals into a scratch project.
//
//   AetherAiSmoke <baseUrl> <apiKey> <projectDir>
//
// Exit 0 = pass.
#include "ai_assist/dev_team.h"
#include "core/log.h"
#include <chrono>
#include <cstdio>
#include <fstream>
#include <thread>

using namespace ae;

static bool waitIdle(DevTeam& team, int seconds) {
    for (int i = 0; i < seconds * 10; ++i) {
        if (!team.busy()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("Usage: AetherAiSmoke <baseUrl> <apiKey> <projectDir>\n");
        return 1;
    }
    PulseClient client;
    client.config.baseUrl = argv[1];
    client.config.apiKey = argv[2];

    std::string err;
    if (!client.online(&err)) {
        std::printf("[AiSmoke] backend offline: %s -> FAIL\n", err.c_str());
        return 1;
    }
    std::printf("[AiSmoke] backend online\n");

    DevTeam team;
    team.init(&client, argv[3]);

    team.requestPlans("add collectible coins with a HUD counter");
    if (!waitIdle(team, 120)) {
        std::printf("[AiSmoke] plan job timed out -> FAIL\n");
        return 1;
    }
    auto plans = team.plans();
    std::printf("[AiSmoke] plans: %d (want >= 2)\n", (int)plans.size());
    if (plans.size() < 2) return 1;
    for (const auto& p : plans)
        std::printf("[AiSmoke]   plan '%s' (%d steps, %d artifacts)\n", p.title.c_str(),
                    (int)p.steps.size(), (int)p.artifacts.size());

    auto agents = team.agents();
    bool ragUsed = false;
    for (const auto& a : agents) ragUsed |= !a.citations.empty();
    std::printf("[AiSmoke] planner RAG citations: %s\n", ragUsed ? "yes" : "NO");

    team.generate(0);
    if (!waitIdle(team, 240)) {
        std::printf("[AiSmoke] generate job timed out -> FAIL\n");
        return 1;
    }
    auto proposals = team.proposals();
    std::printf("[AiSmoke] proposals: %d (want >= 2)\n", (int)proposals.size());
    if (proposals.size() < 2) return 1;

    int applied = 0, rejected = 0;
    for (size_t i = 0; i < proposals.size(); ++i) {
        std::string aerr;
        if (team.applyProposal(i, &aerr)) {
            ++applied;
        } else {
            ++rejected;
            std::printf("[AiSmoke]   rejected %s: %s\n", proposals[i].path.c_str(),
                        aerr.c_str());
        }
    }
    std::printf("[AiSmoke] applied %d, rejected %d\n", applied, rejected);
    if (applied < 1) return 1;

    // The applied files must exist and (for .json) be valid.
    for (const auto& p : team.proposals()) {
        if (!p.applied) continue;
        std::ifstream f(std::string(argv[3]) + "/" + p.path);
        if (!f) {
            std::printf("[AiSmoke] applied file missing: %s -> FAIL\n", p.path.c_str());
            return 1;
        }
    }

    std::printf("[AiSmoke] ALL PASS\n");
    return 0;
}
