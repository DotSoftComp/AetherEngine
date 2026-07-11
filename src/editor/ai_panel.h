// Aether Engine — AI Assistant panel (the PulseLABS dev team, visualized).
//
// The point of this panel is that the user can SEE how the assistant works:
// a live pipeline strip (prompt -> team -> knowledge -> planner -> specialists
// -> review), one card per agent persona with its state, reasoning trace and
// the RAG citations it used, the alternative plans side by side, and every
// proposed file with a preview + explicit Apply - the AI never touches the
// project silently.
#pragma once
#include "../ai_assist/dev_team.h"
#include "../ai_assist/pulse_client.h"
#include <string>

namespace ae {

class AiPanel {
public:
    bool visible = false;

    void init(const std::string& projectRoot);
    void draw();

private:
    void drawConnection();
    void drawPipeline(const std::vector<DevTeam::StageInfo>& stages);
    void drawAgents();
    void drawPlans();
    void drawProposals();

    PulseClient client_;
    DevTeam team_;
    std::string projectRoot_;
    bool initialized_ = false;

    char promptBuf_[1024] = {};
    char urlBuf_[256] = {};
    char keyBuf_[256] = {};
    int connState_ = 0; // 0 unknown, 1 checking?, 2 online, 3 offline
    std::string connError_;
    int previewIndex_ = -1;
    bool setupOpen_ = false;
};

} // namespace ae
