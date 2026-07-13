#include "ai_panel.h"
#include "../core/log.h"
#include "imgui.h"
#include <cmath>
#include <cstring>

namespace ae {

namespace {

ImU32 stateColor(DevTeam::State s, float pulse) {
    switch (s) {
    case DevTeam::State::Running: {
        int g = (int)(140 + 80 * pulse);
        return IM_COL32(80, g, 245, 255);
    }
    case DevTeam::State::Done: return IM_COL32(90, 200, 120, 255);
    case DevTeam::State::Error: return IM_COL32(235, 90, 90, 255);
    default: return IM_COL32(90, 92, 104, 255);
    }
}

const char* kStageNames[DevTeam::StageCount] = {"Prompt", "Team",       "Knowledge",
                                                "Planner", "Specialists", "Review"};

} // namespace

void AiPanel::init(const std::string& projectRoot) {
    projectRoot_ = projectRoot;
    client_.config = PulseConfig::load();
    std::snprintf(urlBuf_, sizeof(urlBuf_), "%s", client_.config.baseUrl.c_str());
    std::snprintf(keyBuf_, sizeof(keyBuf_), "%s", client_.config.apiKey.c_str());
    team_.init(&client_, projectRoot);
    team_.onCppApplied = [this](const std::string& path) {
        if (!onCompileScripts) return;
        AE_LOG("[AI] C++ proposal applied (%s) — compiling scripts", path.c_str());
        onCompileScripts();
    };
    setupOpen_ = !client_.config.configured();
    initialized_ = true;
}

void AiPanel::drawConnection() {
    // Status dot + one-line summary, setup folded away once configured.
    const char* label = connState_ == 2   ? "online"
                        : connState_ == 3 ? "offline"
                                          : "not checked";
    ImU32 dot = connState_ == 2   ? IM_COL32(90, 200, 120, 255)
                : connState_ == 3 ? IM_COL32(235, 90, 90, 255)
                                  : IM_COL32(150, 152, 160, 255);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + 6, p.y + ImGui::GetTextLineHeight() * 0.55f), 5.0f, dot);
    ImGui::Dummy(ImVec2(16, 0));
    ImGui::SameLine();
    ImGui::Text("PulseLABS backend: %s", label);
    if (connState_ == 3 && !connError_.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("- %s", connError_.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Test")) {
        connError_.clear();
        connState_ = client_.online(&connError_) ? 2 : 3;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(setupOpen_ ? "Hide setup" : "Setup")) setupOpen_ = !setupOpen_;

    if (setupOpen_) {
        ImGui::Indent();
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("Backend URL", urlBuf_, sizeof(urlBuf_));
        ImGui::SetNextItemWidth(320);
        ImGui::InputText("API key", keyBuf_, sizeof(keyBuf_), ImGuiInputTextFlags_Password);
        ImGui::SetNextItemWidth(320);
        ImGui::InputInt("Request timeout (s)", &client_.config.timeoutSeconds, 30, 120);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
            ImGui::SetTooltip("How long to wait on the model for one plan/generation call.\n"
                              "Local models need generous values (default 600s = 10 min).");
        ImGui::TextDisabled("Self-hosted PulseLABS default: http://localhost:3051 - create a "
                            "key in its Developer Portal.");
        if (ImGui::Button("Save & test")) {
            client_.config.baseUrl = urlBuf_;
            client_.config.apiKey = keyBuf_;
            if (client_.config.timeoutSeconds < 30) client_.config.timeoutSeconds = 30;
            client_.config.save();
            connError_.clear();
            connState_ = client_.online(&connError_) ? 2 : 3;
            if (connState_ == 2) setupOpen_ = false;
        }
        ImGui::Unindent();
    }
}

void AiPanel::drawPipeline(const std::vector<DevTeam::StageInfo>& stages) {
    float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float avail = ImGui::GetContentRegionAvail().x;
    float gap = 18.0f;
    float nodeW = (avail - gap * (DevTeam::StageCount - 1)) / DevTeam::StageCount;
    if (nodeW < 70) nodeW = 70;
    float nodeH = 34.0f;

    for (int i = 0; i < DevTeam::StageCount; ++i) {
        float x = origin.x + i * (nodeW + gap);
        ImVec2 a(x, origin.y), b(x + nodeW, origin.y + nodeH);
        ImU32 col = stateColor(stages[i].state, pulse);
        dl->AddRectFilled(a, b, (col & 0x00FFFFFF) | 0x30000000, 6.0f);
        dl->AddRect(a, b, col, 6.0f, 0, stages[i].state == DevTeam::State::Running ? 2.5f : 1.2f);
        ImVec2 ts = ImGui::CalcTextSize(kStageNames[i]);
        dl->AddText(ImVec2(x + (nodeW - ts.x) * 0.5f, origin.y + (nodeH - ts.y) * 0.5f),
                    IM_COL32(230, 232, 238, 255), kStageNames[i]);
        if (i + 1 < DevTeam::StageCount) {
            float ay = origin.y + nodeH * 0.5f;
            dl->AddLine(ImVec2(b.x + 3, ay), ImVec2(b.x + gap - 6, ay),
                        IM_COL32(120, 122, 132, 255), 1.5f);
            dl->AddTriangleFilled(ImVec2(b.x + gap - 6, ay - 4), ImVec2(b.x + gap - 6, ay + 4),
                                  ImVec2(b.x + gap - 1, ay), IM_COL32(120, 122, 132, 255));
        }
        if (!stages[i].detail.empty()) {
            ImGui::SetCursorScreenPos(ImVec2(x, origin.y + nodeH + 3));
            ImGui::PushTextWrapPos(x + nodeW);
            ImGui::TextDisabled("%s", stages[i].detail.c_str());
            ImGui::PopTextWrapPos();
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + nodeH + 40));
}

void AiPanel::drawAgents() {
    float pulse = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 5.0f);
    std::vector<DevTeam::AgentSlot> agents = team_.agents();
    float cardW = (ImGui::GetContentRegionAvail().x - 16) / (float)agents.size();
    for (size_t i = 0; i < agents.size(); ++i) {
        const DevTeam::AgentSlot& a = agents[i];
        if (i) ImGui::SameLine();
        ImGui::BeginChild((std::string("agent") + a.role).c_str(), ImVec2(cardW, 96), true);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(p.x + 5, p.y + 8), 4.5f, stateColor(a.state, pulse));
        ImGui::Dummy(ImVec2(13, 0));
        ImGui::SameLine();
        ImGui::TextUnformatted(a.title.c_str());
        ImGui::TextDisabled(a.id.empty() ? "(not created yet)" : "persona: %s",
                            a.id.substr(0, 8).c_str());
        if (!a.citations.empty()) {
            ImGui::TextDisabled("RAG: %d chunks", (int)a.citations.size());
            if (ImGui::IsItemHovered()) {
                std::string tip;
                for (const std::string& c : a.citations) tip += c + "\n";
                ImGui::SetTooltip("%s", tip.c_str());
            }
        }
        if (!a.thoughts.empty() && ImGui::SmallButton("thoughts"))
            ImGui::OpenPopup("thoughts");
        if (ImGui::BeginPopup("thoughts")) {
            ImGui::PushTextWrapPos(420);
            ImGui::TextUnformatted(a.thoughts.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndPopup();
        }
        // Raw model output — the fastest way to see WHY a response failed to
        // parse (fenced JSON, prose wrapper, truncation, refusal, ...).
        if (!a.lastRaw.empty()) {
            if (a.state == DevTeam::State::Error) ImGui::SameLine();
            if (ImGui::SmallButton("raw")) ImGui::OpenPopup("raw");
        }
        if (ImGui::BeginPopup("raw")) {
            ImGui::TextDisabled("%s — raw response (%d chars)", a.title.c_str(),
                                (int)a.lastRaw.size());
            if (ImGui::SmallButton("Copy")) ImGui::SetClipboardText(a.lastRaw.c_str());
            ImGui::Separator();
            ImGui::BeginChild("rawtext", ImVec2(560, 320), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(a.lastRaw.c_str());
            ImGui::EndChild();
            ImGui::EndPopup();
        }
        ImGui::EndChild();
    }
}

void AiPanel::drawPlans() {
    std::vector<DevTeam::Plan> plans = team_.plans();
    if (plans.empty()) return;
    ImGui::SeparatorText("Alternative plans - pick one");
    int selected = team_.selectedPlan();
    for (size_t i = 0; i < plans.size(); ++i) {
        const DevTeam::Plan& p = plans[i];
        ImGui::PushID((int)i);
        bool isSel = (int)i == selected;
        if (isSel)
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.16f, 0.20f, 0.32f, 1.0f));
        ImGui::BeginChild("plan", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
        ImGui::Text("%s", p.title.c_str());
        ImGui::PushTextWrapPos();
        ImGui::TextDisabled("%s", p.summary.c_str());
        ImGui::PopTextWrapPos();
        for (const DevTeam::PlanStep& s : p.steps) {
            if (s.detail.empty())
                ImGui::BulletText("[%s] %s", s.agent.c_str(), s.title.c_str());
            else
                ImGui::BulletText("[%s] %s - %s", s.agent.c_str(), s.title.c_str(),
                                  s.detail.c_str());
        }
        if (!p.artifacts.empty()) {
            std::string arts;
            for (const std::string& a : p.artifacts) arts += (arts.empty() ? "" : ", ") + a;
            ImGui::TextDisabled("creates: %s", arts.c_str());
        }
        ImGui::BeginDisabled(team_.busy());
        if (ImGui::Button(isSel ? "Selected" : "Generate with this plan"))
            team_.generate((int)i);
        ImGui::EndDisabled();
        ImGui::EndChild();
        if (isSel) ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }
}

void AiPanel::drawProposals() {
    std::vector<DevTeam::Proposal> proposals = team_.proposals();
    if (proposals.empty()) return;
    ImGui::SeparatorText("Proposed files - nothing is written until you apply");

    if (ImGui::Button("Apply all valid")) {
        for (size_t i = 0; i < proposals.size(); ++i) {
            if (proposals[i].applied || !proposals[i].jsonValid || !proposals[i].issues.empty())
                continue;
            std::string err;
            if (!team_.applyProposal(i, &err)) AE_ERROR("[AI] %s", err.c_str());
        }
    }
    // If validation found format problems, offer to send them straight back to
    // the specialists (they revise in-session) — the self-correction loop.
    std::string allIssues;
    for (const auto& p : proposals)
        if (!p.applied && !p.issues.empty())
            allIssues += p.path + ": " + p.issues + "\n";
    if (!allIssues.empty()) {
        ImGui::SameLine();
        ImGui::BeginDisabled(team_.busy());
        if (ImGui::Button("Fix format issues")) {
            team_.refine("These files don't match the engine's format. Fix exactly these "
                         "problems, keeping the same JSON output shape:\n" + allIssues +
                         "Match the example files you were given.");
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(!) format issues found");
    }

    for (size_t i = 0; i < proposals.size(); ++i) {
        const DevTeam::Proposal& p = proposals[i];
        ImGui::PushID((int)i);
        const char* status = p.applied ? "applied" : (!p.jsonValid ? "INVALID JSON" : (p.exists ? "overwrites!" : "new"));
        ImGui::Bullet();
        ImGui::SameLine();
        ImGui::Text("%s", p.path.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("[%s, %s, by %s] %s", p.kind.c_str(), status, p.agent.c_str(),
                            p.description.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(previewIndex_ == (int)i ? "hide" : "preview"))
            previewIndex_ = previewIndex_ == (int)i ? -1 : (int)i;
        ImGui::SameLine();
        ImGui::BeginDisabled(p.applied || !p.jsonValid);
        if (ImGui::SmallButton("apply")) {
            std::string err;
            if (!team_.applyProposal(i, &err)) AE_ERROR("[AI] %s", err.c_str());
        }
        ImGui::EndDisabled();
        if (!p.issues.empty()) {
            ImGui::Indent();
            ImGui::PushTextWrapPos();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "format: %s", p.issues.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Unindent();
        }
        if (previewIndex_ == (int)i) {
            ImGui::BeginChild("preview", ImVec2(0, 220), true,
                              ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::TextUnformatted(p.content.c_str());
            ImGui::EndChild();
        }
        ImGui::PopID();
    }

    // Multi-turn refinement: feedback goes back into the specialists' SAME
    // sessions; revised files replace the proposals above.
    ImGui::Spacing();
    ImGui::SetNextItemWidth(-150);
    bool sendFb = ImGui::InputTextWithHint("##feedback",
                                           "Not quite right? Describe the changes...",
                                           feedbackBuf_, sizeof(feedbackBuf_),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(team_.busy() || !feedbackBuf_[0]);
    if (ImGui::Button("Request changes", ImVec2(140, 0)) || (sendFb && feedbackBuf_[0])) {
        team_.refine(feedbackBuf_);
        feedbackBuf_[0] = 0;
    }
    ImGui::EndDisabled();

    // Live-editor calls the personas asked for — explicit, like everything.
    std::vector<DevTeam::BridgeCall> calls = team_.bridgeCalls();
    if (!calls.empty()) {
        ImGui::SeparatorText("Requested live-editor calls (agent bridge)");
        int pending = 0;
        for (size_t i = 0; i < calls.size(); ++i) {
            const DevTeam::BridgeCall& c = calls[i];
            ImGui::PushID((int)(1000 + i));
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::Text("%s", c.method.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("[by %s] %s", c.agent.c_str(),
                                c.ran ? (c.ok ? "ok" : "FAILED") : "pending");
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("params: %s%s%s", c.params.c_str(),
                                  c.ran ? "\nresult: " : "",
                                  c.ran ? c.result.c_str() : "");
            if (!c.ran) ++pending;
            ImGui::PopID();
        }
        ImGui::BeginDisabled(team_.busy() || pending == 0);
        char runLabel[64];
        std::snprintf(runLabel, sizeof(runLabel), "Run %d call(s) on the live editor", pending);
        if (ImGui::Button(runLabel)) team_.runBridgeCalls();
        ImGui::EndDisabled();
    }
}

void AiPanel::draw() {
    if (!initialized_) return;
    ImGui::SetNextWindowSize(ImVec2(760, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("AI Assistant", &visible)) {
        ImGui::End();
        return;
    }

    drawConnection();
    ImGui::Spacing();

    // Prompt
    ImGui::SetNextItemWidth(-130);
    bool go = ImGui::InputTextWithHint("##prompt", "Describe what to build - one prompt in...",
                                       promptBuf_, sizeof(promptBuf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(team_.busy() || !client_.config.configured() || !promptBuf_[0]);
    if (ImGui::Button("Plan it", ImVec2(120, 0)) || (go && promptBuf_[0])) {
        team_.requestPlans(promptBuf_);
    }
    ImGui::EndDisabled();
    ImGui::Spacing();

    drawPipeline(team_.stages());
    drawAgents();
    ImGui::Spacing();
    drawPlans();
    drawProposals();

    std::vector<std::string> log = team_.log();
    if (!log.empty()) {
        ImGui::SeparatorText("Activity");
        ImGui::BeginChild("ailog", ImVec2(0, 110), true);
        for (const std::string& l : log) ImGui::TextDisabled("%s", l.c_str());
        if (team_.busy()) ImGui::TextDisabled("...");
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace ae
