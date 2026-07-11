#include "mission_panel.h"
#include "../engine/world.h"
#include "../core/log.h"
#include "imgui.h"
#include <cstdio>

namespace ae {

namespace {

void inputText(const char* label, std::string& value, float width = -1.0f) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
}

const char* stateName(MissionState s) {
    switch (s) {
    case MissionState::Active: return "ACTIVE";
    case MissionState::Complete: return "COMPLETE";
    case MissionState::Failed: return "FAILED";
    default: return "not started";
    }
}

ImVec4 stateColor(ObjectiveState s) {
    switch (s) {
    case ObjectiveState::Active: return ImVec4(0.60f, 0.50f, 0.98f, 1.0f);
    case ObjectiveState::Complete: return ImVec4(0.35f, 0.85f, 0.50f, 1.0f);
    case ObjectiveState::Failed: return ImVec4(0.92f, 0.35f, 0.35f, 1.0f);
    default: return ImVec4(0.45f, 0.47f, 0.52f, 1.0f);
    }
}

} // namespace

void MissionPanel::draw(World& world, const std::string& missionsPath, bool playing) {
    if (!ImGui::Begin("Missions", &visible)) {
        ImGui::End();
        return;
    }
    MissionSystem& ms = world.missions;

    if (ImGui::Button("Save")) {
        if (ms.save(missionsPath)) AE_LOG("[Mission] saved %s", missionsPath.c_str());
        else AE_ERROR("[Mission] save failed: %s", missionsPath.c_str());
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) ms.load(missionsPath);
    ImGui::SameLine();
    ImGui::TextDisabled("assets/missions/missions.json");
    ImGui::Separator();

    // Mission list.
    ImGui::BeginChild("##list", ImVec2(190, 0), ImGuiChildFlags_ResizeX);
    for (int i = 0; i < (int)ms.missions.size(); ++i) {
        Mission& m = ms.missions[i];
        ImGui::PushID(i);
        std::string label = m.name.empty() ? (m.id.empty() ? "(mission)" : m.id) : m.name;
        if (playing) {
            ImGui::TextColored(m.state == MissionState::Active   ? ImVec4(0.6f, 0.5f, 1.0f, 1)
                               : m.state == MissionState::Complete ? ImVec4(0.35f, 0.85f, 0.5f, 1)
                                                                   : ImVec4(0.45f, 0.47f, 0.52f, 1),
                               "*");
            ImGui::SameLine();
        }
        if (ImGui::Selectable(label.c_str(), selected_ == i)) selected_ = i;
        ImGui::PopID();
    }
    ImGui::Spacing();
    if (ImGui::Button("+ Add Mission", ImVec2(-1, 0))) {
        Mission m;
        m.id = "mission_" + std::to_string(ms.missions.size() + 1);
        m.name = "New Mission";
        ms.missions.push_back(m);
        selected_ = (int)ms.missions.size() - 1;
    }
    ImGui::EndChild();
    ImGui::SameLine();

    // Selected mission editor.
    ImGui::BeginChild("##props");
    if (selected_ >= 0 && selected_ < (int)ms.missions.size()) {
        Mission& m = ms.missions[selected_];
        inputText("Id", m.id, 200);
        inputText("Name", m.name, 260);
        char desc[512];
        std::snprintf(desc, sizeof(desc), "%s", m.description.c_str());
        if (ImGui::InputTextMultiline("Description", desc, sizeof(desc), ImVec2(-1, 54)))
            m.description = desc;
        ImGui::Checkbox("Auto start", &m.autoStart);
        ImGui::SameLine();
        ImGui::Checkbox("Sequential objectives", &m.sequential);
        if (playing) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.5f, 1.0f, 1.0f), "[%s]", stateName(m.state));
            if (m.state == MissionState::NotStarted) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Start now")) ms.startMission(m.id);
            }
        }

        ImGui::SeparatorText("Objectives");
        int removeAt = -1;
        for (int i = 0; i < (int)m.objectives.size(); ++i) {
            Objective& o = m.objectives[i];
            ImGui::PushID(i);
            char hdr[160];
            std::snprintf(hdr, sizeof(hdr), "%d. %s###obj", i + 1,
                          o.text.empty() ? o.id.c_str() : o.text.c_str());
            if (playing) {
                ImGui::TextColored(stateColor(o.state), "o");
                ImGui::SameLine();
            }
            if (ImGui::TreeNodeEx(hdr, ImGuiTreeNodeFlags_DefaultOpen)) {
                inputText("Id##obj", o.id, 160);
                inputText("Text (HUD)", o.text, 260);
                int type = o.type == ObjectiveType::Flag ? 1 : 0;
                if (ImGui::Combo("Type", &type, "Reach entity\0Story flag\0"))
                    o.type = type == 1 ? ObjectiveType::Flag : ObjectiveType::Reach;
                if (o.type == ObjectiveType::Reach) {
                    inputText("Target entity", o.targetEntity, 200);
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragFloat("Radius", &o.radius, 0.05f, 0.1f, 100.0f);
                } else {
                    inputText("Flag", o.flag, 200);
                    ImGui::SetNextItemWidth(120);
                    ImGui::DragInt("Value", &o.flagValue, 0.1f);
                }
                ImGui::Checkbox("Optional", &o.optional);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) removeAt = i;
                ImGui::TreePop();
            }
            ImGui::PopID();
        }
        if (removeAt >= 0) m.objectives.erase(m.objectives.begin() + removeAt);
        if (ImGui::Button("+ Add Objective")) {
            Objective o;
            o.id = "objective_" + std::to_string(m.objectives.size() + 1);
            o.text = "New objective";
            m.objectives.push_back(o);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Mission")) {
            ms.missions.erase(ms.missions.begin() + selected_);
            selected_ = -1;
        }
    } else {
        ImGui::TextDisabled("Select or add a mission.");
    }

    // Live story flags during Play.
    if (playing) {
        ImGui::SeparatorText("Story flags (live)");
        if (ms.flags().empty()) ImGui::TextDisabled("(none set yet)");
        for (const auto& kv : ms.flags())
            ImGui::Text("%s = %d", kv.first.c_str(), kv.second);
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace ae
