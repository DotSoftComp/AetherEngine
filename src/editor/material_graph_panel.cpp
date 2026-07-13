// Aether Engine — material-graph node canvas (see material_graph_panel.h).
#include "material_graph_panel.h"
#include "../engine/world.h"
#include "../engine/entity.h"
#include "../engine/components.h"
#include "../engine/assets.h"
#include "../core/log.h"
#include "../core/math3d.h"
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ae {

// A node offers an output pin iff it can emit an expression (Output and
// SubOutput are pure sinks).
static bool hasOutPin(const MGNodeDef* d) { return d && d->emit != nullptr; }

bool MaterialGraphPanel::createStarterGraph(const std::string& path) {
    MaterialGraph g;
    MGNode out;
    out.id = "output";
    out.type = "Output";
    out.in = {"col", "", "", "", "", ""};
    out.x = 420;
    out.y = 100;
    MGNode col;
    col.id = "col";
    col.type = "Color";
    col.v = Vec3(0.8f, 0.35f, 0.1f);
    col.x = 120;
    col.y = 100;
    g.nodes.push_back(col);
    g.nodes.push_back(out);
    return saveMaterialGraph(g, path);
}

void MaterialGraphPanel::open(const std::string& path, bool focus) {
    MaterialGraph g;
    if (!loadMaterialGraph(g, path)) return;
    graph_ = std::move(g);
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    selected_ = -1;
    linkNode_ = -1;
    if (focus) {
        visible = true;
        focusRequested_ = true;
    }
}

bool MaterialGraphPanel::save(World* world, AssetLibrary* assets) {
    if (!saveMaterialGraph(graph_, path_)) return false;
    dirty_ = false;
    if (assets) {
        assets->reloadMaterialGraph(path_); // recompile the cached shader in place
        if (world) { // re-resolve components that reference the file
            for (const auto& e : world->entities())
                for (const auto& c : e->components())
                    if (auto* mr = dynamic_cast<MeshRenderer*>(c.get()))
                        if (!mr->material.graphPath.empty() &&
                            assets->resolvePath(mr->material.graphPath) == path_)
                            mr->onDeserialized(*assets);
        }
    }
    return true;
}

void MaterialGraphPanel::addNode(const std::string& type, float x, float y) {
    const MGNodeDef* def = materialNodeDef(type);
    if (!def) return;
    if (type == "Output" && graph_.outputNode() >= 0) return; // one Output per graph
    MGNode n;
    n.type = type;
    n.x = x;
    n.y = y;
    n.in.assign(def->in.size(), "");
    if (type == "Color") n.v = Vec3(0.8f, 0.8f, 0.8f);
    if (type == "UV") n.v = Vec3(1, 1, 0);
    for (int i = 1;; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "n%d", i);
        if (graph_.indexOf(id) < 0) { n.id = id; break; }
    }
    graph_.nodes.push_back(std::move(n));
    selected_ = (int)graph_.nodes.size() - 1;
    dirty_ = true;
}

void MaterialGraphPanel::addNodeMenu(float gx, float gy) {
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##filter", "search...", paletteFilter_, sizeof(paletteFilter_));
    std::string filter = paletteFilter_;
    for (auto& ch : filter) ch = (char)std::tolower((unsigned char)ch);

    std::string lastCat;
    for (const auto& d : materialNodeDefs()) {
        if (!filter.empty()) {
            std::string t = d.type;
            for (auto& ch : t) ch = (char)std::tolower((unsigned char)ch);
            if (t.find(filter) == std::string::npos) continue;
        }
        if (d.category != lastCat) {
            if (!lastCat.empty()) ImGui::Separator();
            ImGui::TextDisabled("%s", d.category.c_str());
            lastCat = d.category;
        }
        if (ImGui::MenuItem(d.type.c_str())) {
            addNode(d.type, gx, gy);
            paletteFilter_[0] = 0;
            ImGui::CloseCurrentPopup();
        }
    }
}

void MaterialGraphPanel::draw(World* world, AssetLibrary* assets) {
    if (!visible) return;
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Material Graph", &visible)) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("No material graph loaded.");
        ImGui::TextDisabled("Double-click a .json in assets/materials in the Content Browser,");
        ImGui::TextDisabled("or use Tools > Material Graph Editor.");
        ImGui::End();
        return;
    }
    drawToolbar(world, assets);
    ImGui::Separator();

    const float sidebarW = 300.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas", ImVec2(avail.x - sidebarW - 8.0f, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sidebar");
    drawSidebar();
    ImGui::EndChild();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        save(world, assets);
    ImGui::End();
}

void MaterialGraphPanel::drawToolbar(World* world, AssetLibrary* assets) {
    if (ImGui::Button(dirty_ ? "Save*" : "Save")) save(world, assets);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) open(path_);
    ImGui::SameLine();
    if (ImGui::Button("+ Node")) ImGui::OpenPopup("add_node_toolbar");
    if (ImGui::BeginPopup("add_node_toolbar")) {
        addNodeMenu((200.0f - pan_.x) / zoom_, (120.0f - pan_.y) / zoom_);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %s  ·  %d nodes  ·  zoom %.0f%%", path_.c_str(),
                        (int)graph_.nodes.size(), zoom_ * 100.0f);
}

void MaterialGraphPanel::drawCanvas() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 40 || size.y < 40) return;

    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bg", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO& io = ImGui::GetIO();

    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
    }
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
        float old = zoom_;
        zoom_ = clampf(zoom_ * (1.0f + io.MouseWheel * 0.10f), 0.35f, 2.0f);
        ImVec2 mouse(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        pan_.x = mouse.x - (mouse.x - pan_.x) * (zoom_ / old);
        pan_.y = mouse.y - (mouse.y - pan_.y) * (zoom_ / old);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selected_ = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && linkNode_ >= 0) linkNode_ = -1;

    if (ImGui::BeginPopupContextItem("canvas_ctx")) {
        ImVec2 mp = ImGui::GetMousePosOnOpeningCurrentPopup();
        addNodeMenu((mp.x - origin.x - pan_.x) / zoom_, (mp.y - origin.y - pan_.y) / zoom_);
        ImGui::EndPopup();
    }

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(16, 16, 21, 255));
    float step = 28.0f * zoom_;
    for (float x = std::fmod(pan_.x, step); x < size.x; x += step)
        dl->AddLine(ImVec2(origin.x + x, origin.y), ImVec2(origin.x + x, origin.y + size.y),
                    IM_COL32(255, 255, 255, 7));
    for (float y = std::fmod(pan_.y, step); y < size.y; y += step)
        dl->AddLine(ImVec2(origin.x, origin.y + y), ImVec2(origin.x + size.x, origin.y + y),
                    IM_COL32(255, 255, 255, 7));

    ImFont* font = ImGui::GetFont();
    const float fs = ImGui::GetFontSize() * zoom_;
    const float nodeW = 175.0f * zoom_;
    const float headH = 22.0f * zoom_;
    const float rowH = 18.0f * zoom_;

    auto headColor = [](const MGNodeDef* d) -> ImU32 {
        if (!d) return IM_COL32(120, 120, 130, 255);
        if (d->category == "Output") return IM_COL32(180, 70, 80, 255);
        if (d->category == "Input") return IM_COL32(80, 150, 60, 255);
        if (d->category == "Texture") return IM_COL32(90, 160, 220, 255);
        return IM_COL32(70, 125, 115, 255); // Math
    };
    auto rowsOf = [&](const MGNodeDef* d) {
        return d ? (int)d->in.size() + (hasOutPin(d) ? 1 : 0) : 1;
    };
    auto nodeTopLeft = [&](const MGNode& n) {
        return ImVec2(origin.x + pan_.x + n.x * zoom_, origin.y + pan_.y + n.y * zoom_);
    };
    auto nodeHeight = [&](const MGNode& n) {
        return headH + rowsOf(materialNodeDef(n.type)) * rowH + 6.0f * zoom_;
    };
    auto inPinPos = [&](const MGNode& n, int pin) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x, tl.y + headH + rowH * (pin + 0.5f));
    };
    auto outPinPos = [&](const MGNode& n, const MGNodeDef* d) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x + nodeW, tl.y + headH + rowH * (d->in.size() + 0.5f));
    };

    // Links.
    for (auto& n : graph_.nodes) {
        const MGNodeDef* d = materialNodeDef(n.type);
        if (!d) continue;
        for (int pin = 0; pin < (int)n.in.size(); ++pin) {
            if (n.in[pin].empty()) continue;
            int si = graph_.indexOf(n.in[pin]);
            if (si < 0) continue;
            const MGNodeDef* sd = materialNodeDef(graph_.nodes[si].type);
            if (!sd || !sd->emit) continue;
            ImVec2 a = outPinPos(graph_.nodes[si], sd);
            ImVec2 bpos = inPinPos(n, pin);
            ImU32 col = (ImU32)mgTypeColor(sd->out);
            float bend = std::max(30.0f, std::fabs(bpos.x - a.x) * 0.5f) * zoom_;
            dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(bpos.x - bend, bpos.y), bpos,
                               col, 2.0f * zoom_);
        }
    }
    if (linkNode_ >= 0 && linkNode_ < (int)graph_.nodes.size()) {
        const MGNodeDef* sd = materialNodeDef(graph_.nodes[linkNode_].type);
        if (sd && sd->emit) {
            ImVec2 a = outPinPos(graph_.nodes[linkNode_], sd);
            ImVec2 bpos = io.MousePos;
            dl->AddBezierCubic(a, ImVec2(a.x + 60 * zoom_, a.y),
                               ImVec2(bpos.x - 60 * zoom_, bpos.y), bpos,
                               IM_COL32(255, 255, 255, 160), 2.0f * zoom_);
        }
    }

    // Nodes.
    for (int i = 0; i < (int)graph_.nodes.size(); ++i) {
        MGNode& n = graph_.nodes[i];
        const MGNodeDef* d = materialNodeDef(n.type);
        ImVec2 tl = nodeTopLeft(n);
        float h = nodeHeight(n);
        ImVec2 br(tl.x + nodeW, tl.y + h);

        dl->AddRectFilled(tl, br, IM_COL32(34, 34, 43, 245), 6.0f * zoom_);
        dl->AddRectFilled(tl, ImVec2(br.x, tl.y + headH), headColor(d), 6.0f * zoom_,
                          ImDrawFlags_RoundCornersTop);
        dl->AddText(font, fs * 0.9f, ImVec2(tl.x + 10 * zoom_, tl.y + 3.5f * zoom_),
                    IM_COL32(245, 246, 250, 255), n.type.c_str());

        if (d) {
            float y = tl.y + headH + 2.0f * zoom_;
            for (size_t p = 0; p < d->in.size(); ++p) {
                char row[64];
                std::snprintf(row, sizeof(row), "%s%s", d->in[p].name.c_str(),
                              (p < n.in.size() && !n.in[p].empty()) ? " <-" : "");
                dl->AddText(font, fs * 0.85f, ImVec2(tl.x + 12 * zoom_, y),
                            IM_COL32(190, 193, 202, 255), row);
                y += rowH;
            }
            if (hasOutPin(d)) {
                ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0, "out");
                dl->AddText(font, fs * 0.85f, ImVec2(br.x - ts.x - 12 * zoom_, y),
                            (ImU32)mgTypeColor(d->out), "out");
            }
        }

        if (selected_ == i)
            dl->AddRect(ImVec2(tl.x - 1, tl.y - 1), ImVec2(br.x + 1, br.y + 1),
                        IM_COL32(255, 255, 255, 200), 6.0f * zoom_, 0, 2.0f);

        ImGui::SetCursorScreenPos(tl);
        ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##node", ImVec2(nodeW, h));
        if (ImGui::IsItemActivated()) selected_ = i;
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) &&
            linkNode_ < 0) {
            n.x += io.MouseDelta.x / zoom_;
            n.y += io.MouseDelta.y / zoom_;
            dirty_ = true;
        }
        if (ImGui::BeginPopupContextItem("node_ctx")) {
            selected_ = i;
            bool isOutput = n.type == "Output";
            if (!isOutput && ImGui::MenuItem("Delete node")) {
                std::string dead = n.id;
                for (auto& other : graph_.nodes)
                    for (auto& in : other.in)
                        if (in == dead) in.clear();
                graph_.nodes.erase(graph_.nodes.begin() + i);
                selected_ = -1;
                dirty_ = true;
                ImGui::EndPopup();
                ImGui::PopID();
                dl->PopClipRect();
                return;
            }
            if (isOutput) ImGui::TextDisabled("The Output node cannot be deleted.");
            ImGui::EndPopup();
        }

        if (d) {
            for (int pin = 0; pin < (int)d->in.size(); ++pin) { // input pins
                ImVec2 p = inPinPos(n, pin);
                dl->AddCircleFilled(p, 4.0f * zoom_, (ImU32)mgTypeColor(d->in[pin].type));
                ImGui::SetCursorScreenPos(ImVec2(p.x - 7 * zoom_, p.y - 7 * zoom_));
                ImGui::PushID(pin + 2000);
                if (ImGui::InvisibleButton("##ipin", ImVec2(14 * zoom_, 14 * zoom_))) {
                    if (linkNode_ >= 0 && linkNode_ != i) {
                        if (pin < (int)n.in.size()) {
                            n.in[pin] = graph_.nodes[linkNode_].id;
                            dirty_ = true;
                        }
                        linkNode_ = -1;
                    } else if (pin < (int)n.in.size() && !n.in[pin].empty()) {
                        int src = graph_.indexOf(n.in[pin]);
                        n.in[pin].clear();
                        dirty_ = true;
                        if (src >= 0) linkNode_ = src;
                    }
                }
                if (ImGui::IsItemHovered())
                    dl->AddCircle(p, 6.0f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                ImGui::PopID();
            }
            if (hasOutPin(d)) { // output pin
                ImVec2 p = outPinPos(n, d);
                dl->AddCircleFilled(p, 4.0f * zoom_, (ImU32)mgTypeColor(d->out));
                ImGui::SetCursorScreenPos(ImVec2(p.x - 7 * zoom_, p.y - 7 * zoom_));
                ImGui::PushID(3000);
                if (ImGui::InvisibleButton("##opin", ImVec2(14 * zoom_, 14 * zoom_)))
                    linkNode_ = i;
                if (ImGui::IsItemHovered())
                    dl->AddCircle(p, 6.0f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }

    dl->PopClipRect();
}

void MaterialGraphPanel::drawSidebar() {
    if (selected_ < 0 || selected_ >= (int)graph_.nodes.size()) {
        ImGui::TextDisabled("Select a node.");
        ImGui::Spacing();
        ImGui::TextWrapped("Click a node's output pin, then an input pin, to wire data. "
                           "Green = Float, cyan = Vec2, orange = Vec3 (conversions are "
                           "automatic). Right-click the canvas to add nodes; Ctrl+S saves and "
                           "hot-reloads the material on everything using it.");
        return;
    }
    MGNode& n = graph_.nodes[selected_];
    const MGNodeDef* d = materialNodeDef(n.type);
    ImGui::Text("%s", n.type.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", d ? d->category.c_str() : "?");
    ImGui::Separator();

    if (d && d->pLabel) {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", n.p.c_str());
        if (ImGui::InputText(d->pLabel, buf, sizeof(buf))) { n.p = buf; dirty_ = true; }
        // Texture paths accept a drag from the Content Browser.
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* pay = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
                n.p = std::string((const char*)pay->Data, pay->DataSize);
                dirty_ = true;
            }
            ImGui::EndDragDropTarget();
        }
    }
    if (d && d->nLabel) {
        float prev = n.n;
        ImGui::DragFloat(d->nLabel, &n.n, 0.05f);
        if (prev != n.n) dirty_ = true;
    }
    if (d && d->vLabel) {
        Vec3 prev = n.v;
        if (d->vIsColor) ImGui::ColorEdit3(d->vLabel, &n.v.x, ImGuiColorEditFlags_Float);
        else ImGui::DragFloat3(d->vLabel, &n.v.x, 0.05f);
        if (prev.x != n.v.x || prev.y != n.v.y || prev.z != n.v.z) dirty_ = true;
    }

    if (d && !d->in.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Inputs");
        for (int p = 0; p < (int)d->in.size() && p < (int)n.in.size(); ++p) {
            ImGui::Text("%s (%s) <- %s", d->in[p].name.c_str(), mgTypeName(d->in[p].type),
                        n.in[p].empty() ? "(default)" : n.in[p].c_str());
            if (!n.in[p].empty()) {
                ImGui::SameLine();
                ImGui::PushID(p);
                if (ImGui::SmallButton("x")) { n.in[p].clear(); dirty_ = true; }
                ImGui::PopID();
            }
        }
    }
}

} // namespace ae
