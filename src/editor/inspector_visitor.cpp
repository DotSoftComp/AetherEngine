#include "inspector_visitor.h"
#include "../engine/assets.h"
#include "../engine/components.h"
#include "../engine/entity.h"
#include "../engine/world.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>

namespace ae {

static const char* labelOf(const char* key, const PropMeta& m) {
    return m.label ? m.label : key;
}

void ImGuiInspectorVisitor::visit(const char* key, float& v, const PropMeta& m) {
    const char* label = labelOf(key, m);
    switch (m.kind) {
    case PropKind::SliderNorm: {
        float lo = m.min < m.max ? m.min : 0.0f;
        float hi = m.min < m.max ? m.max : 1.0f;
        ImGui::SliderFloat(label, &v, lo, hi);
        break;
    }
    case PropKind::Angle:
        if (m.min < m.max) ImGui::SliderFloat(label, &v, m.min, m.max, "%.0f\xC2\xB0");
        else ImGui::DragFloat(label, &v, m.speed, 0.0f, 0.0f, "%.1f\xC2\xB0");
        break;
    default:
        ImGui::DragFloat(label, &v, m.speed, m.min, m.max);
        break;
    }
}

void ImGuiInspectorVisitor::visit(const char* key, int& v, const PropMeta& m) {
    ImGui::DragInt(labelOf(key, m), &v, m.speed < 1.0f ? 1.0f : m.speed, (int)m.min, (int)m.max);
}

void ImGuiInspectorVisitor::visit(const char* key, bool& v, const PropMeta& m) {
    const char* label = labelOf(key, m);
    if (m.kind == PropKind::CameraDefaultFlag) {
        // Exclusive: at most one default gameplay camera per world.
        if (ImGui::Checkbox(label, &v) && v && world_) {
            for (const auto& other : world_->entities())
                if (auto* oc = other->getComponent<CameraComponent>())
                    if (static_cast<Component*>(oc) != current_) oc->isDefault = false;
        }
        return;
    }
    ImGui::Checkbox(label, &v);
}

void ImGuiInspectorVisitor::visit(const char* key, std::string& v, const PropMeta& m) {
    const char* label = labelOf(key, m);
    switch (m.kind) {
    case PropKind::MeshName: {
        if (ImGui::BeginCombo(label, v.empty() ? "(custom)" : v.c_str())) {
            for (const std::string& n : assets_->meshNames()) {
                if (ImGui::Selectable(n.c_str(), n == v)) {
                    v = n;
                    assetsDirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case PropKind::TexSetName: {
        if (ImGui::BeginCombo(label, v.empty() ? "(none)" : v.c_str())) {
            if (ImGui::Selectable("(none)", v.empty())) {
                v.clear();
                assetsDirty_ = true;
            }
            for (const std::string& n : assets_->textureSetNames()) {
                if (ImGui::Selectable(n.c_str(), n == v)) {
                    v = n;
                    assetsDirty_ = true;
                }
            }
            ImGui::EndCombo();
        }
        break;
    }
    case PropKind::ModelPath:
        ImGui::TextDisabled("%s", v.empty() ? "(unsaved model reference)" : v.c_str());
        break;
    case PropKind::ScriptGraphPath: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = buf;
            assetsDirty_ = true;
        }
        // Accept a script .json dragged from the Content Browser.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                std::string rel((const char*)p->Data, p->DataSize);
                bool isJson =
                    rel.size() >= 5 && _stricmp(rel.c_str() + rel.size() - 5, ".json") == 0;
                if (isJson) { v = rel; assetsDirty_ = true; }
            }
            ImGui::EndDragDropTarget();
        }
        if (openScriptGraph) {
            if (ImGui::Button("Open in Script Graph") && !v.empty()) openScriptGraph(v);
        }
        break;
    }
    case PropKind::AnimGraphPath: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = buf;
            assetsDirty_ = true;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                std::string rel((const char*)p->Data, p->DataSize);
                bool isJson =
                    rel.size() >= 5 && _stricmp(rel.c_str() + rel.size() - 5, ".json") == 0;
                if (isJson) { v = rel; assetsDirty_ = true; }
            }
            ImGui::EndDragDropTarget();
        }
        if (openAnimGraph) {
            if (ImGui::Button("Open in Anim Graph") && !v.empty()) openAnimGraph(v);
        }
        break;
    }
    case PropKind::UIDocPath: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = buf;
            assetsDirty_ = true;
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                std::string rel((const char*)p->Data, p->DataSize);
                bool isJson =
                    rel.size() >= 5 && _stricmp(rel.c_str() + rel.size() - 5, ".json") == 0;
                if (isJson) { v = rel; assetsDirty_ = true; }
            }
            ImGui::EndDragDropTarget();
        }
        if (openUIDesigner) {
            if (ImGui::Button("Open in UI Designer") && !v.empty()) openUIDesigner(v);
        }
        break;
    }
    case PropKind::MaterialGraphPath: {
        std::string base = v;
        size_t slash = v.find_last_of("/\\");
        if (slash != std::string::npos) base = v.substr(slash + 1);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::Button(v.empty() ? "  drop a material graph here  " : base.c_str(),
                      ImVec2(v.empty() ? -1.0f : 0.0f, 0));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                std::string rel((const char*)p->Data, p->DataSize);
                bool isJson =
                    rel.size() >= 5 && _stricmp(rel.c_str() + rel.size() - 5, ".json") == 0;
                if (isJson) { v = rel; assetsDirty_ = true; }
            }
            ImGui::EndDragDropTarget();
        }
        if (!v.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x##mg")) { v.clear(); assetsDirty_ = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear material graph");
            if (openMaterialGraph) {
                ImGui::SameLine();
                if (ImGui::SmallButton("Open")) openMaterialGraph(v);
            }
        }
        break;
    }
    case PropKind::AudioClip: {
        // Drag a .wav from the Content Browser onto this button to assign it.
        std::string base = v;
        size_t slash = v.find_last_of("/\\");
        if (slash != std::string::npos) base = v.substr(slash + 1);
        ImGui::TextUnformatted(label);
        ImGui::SameLine();
        ImGui::Button(v.empty() ? "  drop a .wav here  " : base.c_str(), ImVec2(-1, 0));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                std::string rel((const char*)p->Data, p->DataSize);
                bool isWav = rel.size() >= 4 && _stricmp(rel.c_str() + rel.size() - 4, ".wav") == 0;
                if (isWav) { v = rel; assetsDirty_ = true; }
            }
            ImGui::EndDragDropTarget();
        }
        if (!v.empty()) {
            ImGui::SameLine();
            if (ImGui::SmallButton("x")) { v.clear(); assetsDirty_ = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear clip");
        }
        break;
    }
    case PropKind::DialogueScenePath: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            v = buf;
            assetsDirty_ = true;
        }
        if (openDialogueScene) {
            if (ImGui::Button("Open in Dialogue Graph") && !v.empty()) openDialogueScene(v);
        }
        break;
    }
    case PropKind::Multiline: {
        char buf[1024];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputTextMultiline(label, buf, sizeof(buf))) v = buf;
        break;
    }
    default: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) v = buf;
        break;
    }
    }
}

void ImGuiInspectorVisitor::visit(const char* key, Vec3& v, const PropMeta& m) {
    const char* label = labelOf(key, m);
    switch (m.kind) {
    case PropKind::Color:
        ImGui::ColorEdit3(label, &v.x, ImGuiColorEditFlags_Float);
        break;
    case PropKind::HdrColor:
        ImGui::ColorEdit3(label, &v.x, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        break;
    case PropKind::Radiance:
        ImGui::DragFloat3(label, &v.x, m.speed, m.min, m.max);
        break;
    default:
        ImGui::DragFloat3(label, &v.x, m.speed, m.min, m.max);
        break;
    }
}

void ImGuiInspectorVisitor::visit(const char* key, Vec4& v, const PropMeta& m) {
    const char* label = labelOf(key, m);
    if (m.kind == PropKind::Color)
        ImGui::ColorEdit4(label, &v.x, ImGuiColorEditFlags_Float);
    else
        ImGui::DragFloat4(label, &v.x, m.speed, m.min, m.max);
}

void ImGuiInspectorVisitor::visit(const char*, const char*, EntityRef& ref,
                                  std::string& legacyName, const PropMeta& m) {
    const char* label = m.label ? m.label : "Target";
    Entity* cur = world_ ? ref.get(*world_) : nullptr;
    if (ImGui::BeginCombo(label, cur ? cur->name().c_str() : "(none)")) {
        if (ImGui::Selectable("(none)", !cur)) {
            ref = EntityRef();
            legacyName.clear();
        }
        for (const auto& e : world_->entities()) {
            ImGui::PushID((int)e->id());
            if (ImGui::Selectable(e->name().c_str(), cur == e.get())) {
                ref.set(e.get());
                legacyName = e->name();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void ImGuiInspectorVisitor::beginGroup(const char* key) {
    ImGui::SeparatorText(key);
}

void ImGuiInspectorVisitor::endGroup() {}

} // namespace ae
