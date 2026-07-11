// Aether Engine — UI Designer (see ui_designer_panel.h).
#include "ui_designer_panel.h"
#include "../engine/world.h"
#include "../engine/entity.h"
#include "../engine/assets.h"
#include "../ui/ui_document_component.h"
#include "../core/log.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace ae {

bool UIDesignerPanel::createStarterDoc(const std::string& path) {
    UIDocument doc;
    UIWidget title;
    title.id = "title";
    title.type = "Label";
    title.anchor = Vec2(0.5f, 0.0f);
    title.pivot = Vec2(0.5f, 0.0f);
    title.offset = Vec2(0, 24);
    title.size = Vec2(420, 34);
    title.text = "NEW HUD";
    title.color = Vec4(1, 1, 1, 1);
    title.bg = Vec4(0, 0, 0, 0.45f);
    doc.root.children.push_back(title);
    return saveUIDocument(doc, path);
}

void UIDesignerPanel::open(const std::string& path, bool focus) {
    UIDocument d;
    if (!loadUIDocument(d, path)) return;
    doc_ = std::move(d);
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    selectedId_.clear();
    if (focus) {
        visible = true;
        focusRequested_ = true;
    }
}

bool UIDesignerPanel::save(World* world, AssetLibrary* assets) {
    if (!saveUIDocument(doc_, path_)) return false;
    dirty_ = false;
    if (world && assets) {
        for (const auto& e : world->entities())
            for (const auto& c : e->components())
                if (auto* u = dynamic_cast<UIDocumentComponent*>(c.get()))
                    if (!u->docPath.empty() && assets->resolvePath(u->docPath) == path_)
                        u->reload(*assets);
    }
    return true;
}

void UIDesignerPanel::addChild(const char* type) {
    UIWidget* parent = selected();
    if (!parent) parent = &doc_.root;
    UIWidget w;
    w.type = type;
    for (;; ++nextId_) {
        char id[24];
        std::snprintf(id, sizeof(id), "%s%d", type, nextId_);
        std::string lower = id;
        for (auto& ch : lower) ch = (char)std::tolower((unsigned char)ch);
        if (!doc_.find(lower)) { w.id = lower; break; }
    }
    if (std::strcmp(type, "Label") == 0) { w.text = "Label"; w.bg = Vec4(0, 0, 0, 0); }
    if (std::strcmp(type, "Button") == 0) {
        w.text = "Button";
        w.bg = Vec4(0.16f, 0.15f, 0.24f, 0.92f);
    }
    if (std::strcmp(type, "ProgressBar") == 0) {
        w.size = Vec2(240, 18);
        w.color = Vec4(0.55f, 0.45f, 0.95f, 1);
        w.bg = Vec4(0.08f, 0.08f, 0.12f, 0.85f);
    }
    if (std::strcmp(type, "Panel") == 0) w.bg = Vec4(0.05f, 0.05f, 0.08f, 0.6f);
    w.offset = Vec2(40, 40);
    parent->children.push_back(std::move(w));
    selectedId_ = parent->children.back().id;
    dirty_ = true;
}

bool UIDesignerPanel::removeWidget(UIWidget& parent, const std::string& id) {
    for (size_t i = 0; i < parent.children.size(); ++i) {
        if (parent.children[i].id == id) {
            parent.children.erase(parent.children.begin() + i);
            return true;
        }
        if (removeWidget(parent.children[i], id)) return true;
    }
    return false;
}

void UIDesignerPanel::draw(World* world, AssetLibrary* assets) {
    if (!visible) return;
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1060, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("UI Designer", &visible)) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("No UI document loaded.");
        ImGui::TextDisabled("Double-click a .json in assets/ui in the Content Browser,");
        ImGui::TextDisabled("or use Tools > UI Designer.");
        ImGui::End();
        return;
    }
    drawToolbar(world, assets);
    ImGui::Separator();

    const float treeW = 200.0f, propsW = 300.0f;
    ImGui::BeginChild("##tree", ImVec2(treeW, 0), ImGuiChildFlags_Borders);
    drawTree(doc_.root);
    ImGui::EndChild();
    ImGui::SameLine();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas", ImVec2(avail.x - propsW - 8.0f, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##props");
    drawProps();
    ImGui::EndChild();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        save(world, assets);
    ImGui::End();
}

void UIDesignerPanel::drawToolbar(World* world, AssetLibrary* assets) {
    if (ImGui::Button(dirty_ ? "Save*" : "Save")) save(world, assets);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) open(path_);
    ImGui::SameLine();
    if (ImGui::Button("+ Widget")) ImGui::OpenPopup("add_widget");
    if (ImGui::BeginPopup("add_widget")) {
        ImGui::TextDisabled("Add as child of %s",
                            selected() ? selected()->id.c_str() : "root");
        ImGui::Separator();
        if (ImGui::MenuItem("Panel")) addChild("Panel");
        if (ImGui::MenuItem("Label")) addChild("Label");
        if (ImGui::MenuItem("Button")) addChild("Button");
        if (ImGui::MenuItem("ProgressBar")) addChild("ProgressBar");
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %s  ·  preview 1600x900", path_.c_str());
}

void UIDesignerPanel::drawTree(UIWidget& w) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_DefaultOpen |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    if (w.children.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
    if (selectedId_ == w.id) flags |= ImGuiTreeNodeFlags_Selected;
    char label[96];
    std::snprintf(label, sizeof(label), "%s  (%s)", w.id.c_str(), w.type.c_str());
    bool openNode = ImGui::TreeNodeEx(w.id.c_str(), flags, "%s", label);
    if (ImGui::IsItemClicked()) selectedId_ = w.id;
    if (ImGui::BeginPopupContextItem()) {
        selectedId_ = w.id;
        if (w.id != "root" && ImGui::MenuItem("Delete")) {
            std::string dead = w.id;
            ImGui::EndPopup();
            if (openNode) ImGui::TreePop();
            removeWidget(doc_.root, dead);
            selectedId_.clear();
            dirty_ = true;
            return;
        }
        ImGui::EndPopup();
    }
    if (openNode) {
        for (auto& c : w.children) drawTree(c);
        ImGui::TreePop();
    }
}

void UIDesignerPanel::drawCanvas() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 60 || size.y < 60) return;

    // Fit a 1600x900 virtual screen into the panel.
    const float vw = 1600.0f, vh = 900.0f;
    float scale = std::min((size.x - 16) / vw, (size.y - 16) / vh);
    ImVec2 s0(origin.x + (size.x - vw * scale) * 0.5f, origin.y + (size.y - vh * scale) * 0.5f);

    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(14, 14, 18, 255));
    dl->AddRectFilled(s0, ImVec2(s0.x + vw * scale, s0.y + vh * scale), IM_COL32(30, 32, 40, 255));
    dl->AddRect(s0, ImVec2(s0.x + vw * scale, s0.y + vh * scale), IM_COL32(90, 90, 110, 255));

    ImGuiIO& io = ImGui::GetIO();
    ImGui::InvisibleButton("##uibg", size);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selectedId_.clear();

    // Recursive preview draw + hit test (front-most wins: children after parent).
    UIWidget* hit = nullptr;
    Rect screen{0, 0, vw, vh};
    std::function<void(UIWidget&, const Rect&)> rec = [&](UIWidget& w, const Rect& parent) {
        // The root always spans the full virtual screen (same rule as runtime).
        Rect r = (&w == &doc_.root) ? parent : uiWidgetRect(w, parent);
        ImVec2 a(s0.x + r.x * scale, s0.y + r.y * scale);
        ImVec2 b(s0.x + (r.x + r.w) * scale, s0.y + (r.y + r.h) * scale);
        if (&w != &doc_.root) {
            auto col = [](const Vec4& c, float boost = 1.0f) {
                return IM_COL32((int)(c.x * 255), (int)(c.y * 255), (int)(c.z * 255),
                                (int)(clampf(c.w * boost, 0.05f, 1.0f) * 255));
            };
            if (w.bg.w > 0.001f || w.type == "Panel")
                dl->AddRectFilled(a, b, col(w.bg.w > 0.001f ? w.bg : Vec4(0.3f, 0.3f, 0.4f, 0.3f)));
            if (w.type == "ProgressBar") {
                dl->AddRectFilled(a, ImVec2(a.x + (b.x - a.x) * 0.6f, b.y), col(w.color));
                dl->AddRect(a, b, IM_COL32(255, 255, 255, 70));
            }
            if (w.type == "Label" || w.type == "Button") {
                ImVec2 ts = ImGui::CalcTextSize(w.text.c_str());
                dl->AddText(ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f),
                            col(Vec4(w.color.x, w.color.y, w.color.z, 1)), w.text.c_str());
            }
            if (w.type == "Button") dl->AddRect(a, b, IM_COL32(255, 255, 255, 90));
            if (selectedId_ == w.id)
                dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1),
                            IM_COL32(255, 220, 90, 255), 0, 0, 2.0f);
            if (io.MousePos.x >= a.x && io.MousePos.x < b.x && io.MousePos.y >= a.y &&
                io.MousePos.y < b.y)
                hit = &w; // later (deeper/front-most) hits overwrite
        }
        for (auto& c : w.children) rec(c, r);
    };
    rec(doc_.root, screen);

    if (hit && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        selectedId_ = hit->id;
    // Drag the selected widget (moves its pixel offset).
    if (!selectedId_.empty() && ImGui::IsItemActive() &&
        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
        if (UIWidget* w = selected()) {
            w->offset.x += io.MouseDelta.x / scale;
            w->offset.y += io.MouseDelta.y / scale;
            dirty_ = true;
        }
    }
}

void UIDesignerPanel::drawProps() {
    UIWidget* w = selected();
    if (!w) {
        ImGui::TextDisabled("Select a widget.");
        ImGui::Spacing();
        ImGui::TextWrapped("Anchor places a widget relative to its parent (0..1); pivot picks "
                           "which point of the widget sits there; offset/size are pixels. "
                           "Labels substitute {flag:name}; ProgressBars bind a flag; Button ids "
                           "fire the script OnUIButton event.");
        return;
    }
    ImGui::Text("%s", w->type.c_str());
    if (w->id != "root") {
        char idBuf[64];
        std::snprintf(idBuf, sizeof(idBuf), "%s", w->id.c_str());
        if (ImGui::InputText("Id", idBuf, sizeof(idBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string ni = idBuf;
            if (!ni.empty() && !doc_.find(ni)) {
                std::string old = w->id;
                w->id = ni;
                if (selectedId_ == old) selectedId_ = ni;
                dirty_ = true;
            }
        }
    }
    ImGui::Separator();

    // 3x3 anchor / pivot presets.
    auto grid = [&](const char* label, Vec2& v) {
        ImGui::TextDisabled("%s", label);
        for (int r = 0; r < 3; ++r) {
            for (int c = 0; c < 3; ++c) {
                if (c) ImGui::SameLine();
                float ax = c * 0.5f, ay = r * 0.5f;
                bool cur = std::fabs(v.x - ax) < 0.01f && std::fabs(v.y - ay) < 0.01f;
                char b[24];
                std::snprintf(b, sizeof(b), "%s##%s%d%d", cur ? "o" : ".", label, r, c);
                if (ImGui::SmallButton(b)) { v = Vec2(ax, ay); dirty_ = true; }
            }
        }
    };
    grid("Anchor", w->anchor);
    ImGui::SameLine(0, 24);
    ImGui::BeginGroup();
    grid("Pivot", w->pivot);
    ImGui::EndGroup();

    if (ImGui::DragFloat2("Offset", &w->offset.x, 1.0f)) dirty_ = true;
    if (ImGui::DragFloat2("Size", &w->size.x, 1.0f, 1.0f, 4000.0f)) dirty_ = true;
    bool vis = w->visible;
    if (ImGui::Checkbox("Visible", &vis)) { w->visible = vis; dirty_ = true; }

    if (w->type == "Label" || w->type == "Button") {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", w->text.c_str());
        if (ImGui::InputText("Text", buf, sizeof(buf))) { w->text = buf; dirty_ = true; }
        if (w->type == "Label") ImGui::TextDisabled("{flag:name} substitutes a story flag");
        if (w->type == "Button") ImGui::TextDisabled("Click fires OnUIButton (id = \"%s\")",
                                                     w->id.c_str());
    }
    if (ImGui::ColorEdit4("Color", &w->color.x, ImGuiColorEditFlags_Float)) dirty_ = true;
    if (ImGui::ColorEdit4("Background", &w->bg.x, ImGuiColorEditFlags_Float)) dirty_ = true;
    if (w->type == "ProgressBar") {
        char fb[128];
        std::snprintf(fb, sizeof(fb), "%s", w->bindFlag.c_str());
        if (ImGui::InputText("Bind flag", fb, sizeof(fb))) { w->bindFlag = fb; dirty_ = true; }
        if (ImGui::DragFloat("Max value", &w->barMax, 0.1f, 0.1f, 100000.0f)) dirty_ = true;
    }
}

} // namespace ae
