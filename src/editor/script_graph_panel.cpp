// Aether Engine — visual-scripting v2 node canvas (see script_graph_panel.h).
#include "script_graph_panel.h"
#include "../engine/world.h"
#include "../engine/entity.h"
#include "../engine/assets.h"
#include "../core/log.h"
#include "../core/math3d.h"
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ae {

static void inputText(const char* label, std::string& value) {
    char buf[260];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
}

bool ScriptGraphPanel::createStarterGraph(const std::string& path) {
    ScriptGraph g;
    ScriptNode start;
    start.id = "start";
    start.type = "OnStart";
    start.execOut = {"hello"};
    start.x = 40;
    start.y = 80;
    ScriptNode log;
    log.id = "hello";
    log.type = "Log";
    log.execOut = {""};
    DataIn msg;
    msg.hasLiteral = true;
    msg.literal = Value::S("Hello from the script graph!");
    log.in = {msg};
    log.x = 300;
    log.y = 80;
    g.nodes.push_back(start);
    g.nodes.push_back(log);
    return saveScriptGraph(g, path);
}

void ScriptGraphPanel::open(const std::string& path, bool focus) {
    ScriptGraph g;
    if (!loadScriptGraph(g, path)) return;
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

bool ScriptGraphPanel::save(World* world, AssetLibrary* assets) {
    if (!saveScriptGraph(graph_, path_)) return false;
    dirty_ = false;
    if (world && assets) {
        for (const auto& e : world->entities())
            for (const auto& c : e->components())
                if (auto* sg = dynamic_cast<ScriptGraphComponent*>(c.get()))
                    if (!sg->graphPath.empty() && assets->resolvePath(sg->graphPath) == path_)
                        sg->reload(*assets);
    }
    return true;
}

void ScriptGraphPanel::addNode(const std::string& type, float x, float y) {
    const NodeDef* def = scriptNodeDef(type);
    if (!def) return;
    ScriptNode n;
    n.type = type;
    n.x = x;
    n.y = y;
    n.execOut.assign(def->execOut.size(), "");
    n.in.assign(def->dataIn.size(), {});
    for (int i = 1;; ++i) {
        char id[16];
        std::snprintf(id, sizeof(id), "n%d", i);
        if (graph_.indexOf(id) < 0) { n.id = id; break; }
    }
    graph_.nodes.push_back(std::move(n));
    selected_ = (int)graph_.nodes.size() - 1;
    dirty_ = true;
}

void ScriptGraphPanel::addNodeMenu(float gx, float gy) {
    ImGui::SetNextItemWidth(180);
    ImGui::InputTextWithHint("##filter", "search...", paletteFilter_, sizeof(paletteFilter_));
    std::string filter = paletteFilter_;
    for (auto& ch : filter) ch = (char)std::tolower((unsigned char)ch);

    std::string lastCat;
    for (const auto& d : scriptNodeDefs()) {
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

// ---------------------------------------------------------------------------
void ScriptGraphPanel::draw(World* world, AssetLibrary* assets) {
    if (!visible) return;
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(1050, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Script Graph", &visible)) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("No script graph loaded.");
        ImGui::TextDisabled("Double-click a .json in assets/scripts in the Content Browser,");
        ImGui::TextDisabled("or use Tools > Script Graph Editor.");
        ImGui::End();
        return;
    }
    drawToolbar(world, assets);
    ImGui::Separator();

    const float sidebarW = 320.0f;
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

void ScriptGraphPanel::drawToolbar(World* world, AssetLibrary* assets) {
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
    ImGui::TextDisabled("| %s  ·  %d nodes  ·  %d vars  ·  zoom %.0f%%", path_.c_str(),
                        (int)graph_.nodes.size(), (int)graph_.variables.size(), zoom_ * 100.0f);
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
void ScriptGraphPanel::drawCanvas() {
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
    const float nodeW = 185.0f * zoom_;
    const float headH = 22.0f * zoom_;
    const float rowH = 18.0f * zoom_;

    // Header color by category.
    auto headColor = [](const NodeDef* d) -> ImU32 {
        if (!d) return IM_COL32(120, 120, 130, 255);
        const std::string& c = d->category;
        if (c == "Events") return IM_COL32(80, 150, 60, 255);
        if (c == "Flow" || c == "Functions") return IM_COL32(230, 160, 70, 255);
        if (c == "Variables") return IM_COL32(90, 160, 220, 255);
        if (c == "Math" || c == "Logic" || c == "Values") return IM_COL32(70, 125, 115, 255);
        return IM_COL32(100, 130, 240, 255); // Entity/Physics/Game/Debug actions
    };

    // Row layout: [exec outs][data ins][data outs], one per row under the header.
    auto rowsOf = [&](const NodeDef* d) {
        return d ? (int)(d->execOut.size() + d->dataIn.size() + d->dataOut.size()) : 1;
    };
    auto nodeTopLeft = [&](const ScriptNode& n) {
        return ImVec2(origin.x + pan_.x + n.x * zoom_, origin.y + pan_.y + n.y * zoom_);
    };
    auto nodeHeight = [&](const ScriptNode& n) {
        return headH + rowsOf(scriptNodeDef(n.type)) * rowH + 6.0f * zoom_;
    };
    auto execInPos = [&](const ScriptNode& n) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x, tl.y + headH * 0.5f);
    };
    auto execOutPos = [&](const ScriptNode& n, int pin) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x + nodeW, tl.y + headH + rowH * (pin + 0.5f));
    };
    auto dataInPos = [&](const ScriptNode& n, const NodeDef* d, int pin) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x, tl.y + headH + rowH * (d->execOut.size() + pin + 0.5f));
    };
    auto dataOutPos = [&](const ScriptNode& n, const NodeDef* d, int pin) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x + nodeW,
                      tl.y + headH + rowH * (d->execOut.size() + d->dataIn.size() + pin + 0.5f));
    };

    // ---- links under the nodes ----
    for (auto& n : graph_.nodes) {
        const NodeDef* d = scriptNodeDef(n.type);
        if (!d) continue;
        for (int pin = 0; pin < (int)n.execOut.size(); ++pin) { // exec: white
            if (n.execOut[pin].empty()) continue;
            int ti = graph_.indexOf(n.execOut[pin]);
            ImVec2 a = execOutPos(n, pin);
            if (ti < 0) {
                dl->AddLine(a, ImVec2(a.x + 26 * zoom_, a.y), IM_COL32(230, 70, 70, 200),
                            2.0f * zoom_);
                continue;
            }
            ImVec2 bpos = execInPos(graph_.nodes[ti]);
            float bend = std::max(40.0f, std::fabs(bpos.x - a.x) * 0.5f) * zoom_;
            dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(bpos.x - bend, bpos.y), bpos,
                               IM_COL32(235, 235, 245, 220), 2.6f * zoom_);
        }
        for (int pin = 0; pin < (int)n.in.size(); ++pin) { // data: colored by type
            const DataIn& di = n.in[pin];
            if (di.fromNode.empty()) continue;
            int si = graph_.indexOf(di.fromNode);
            ImVec2 bpos = dataInPos(n, d, pin);
            if (si < 0) continue;
            const NodeDef* sd = scriptNodeDef(graph_.nodes[si].type);
            if (!sd || di.fromOut >= (int)sd->dataOut.size()) continue;
            ImVec2 a = dataOutPos(graph_.nodes[si], sd, di.fromOut);
            ImU32 col = (ImU32)pinTypeColor(sd->dataOut[di.fromOut].type);
            float bend = std::max(30.0f, std::fabs(bpos.x - a.x) * 0.5f) * zoom_;
            dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(bpos.x - bend, bpos.y), bpos,
                               col, 1.8f * zoom_);
        }
    }
    if (linkNode_ >= 0 && linkNode_ < (int)graph_.nodes.size() &&
        (linkIsExec_ || scriptNodeDef(graph_.nodes[linkNode_].type))) {
        const ScriptNode& n = graph_.nodes[linkNode_];
        const NodeDef* d = scriptNodeDef(n.type);
        ImVec2 a = linkIsExec_ ? execOutPos(n, linkPin_) : dataOutPos(n, d, linkPin_);
        ImVec2 bpos = io.MousePos;
        dl->AddBezierCubic(a, ImVec2(a.x + 60 * zoom_, a.y), ImVec2(bpos.x - 60 * zoom_, bpos.y),
                           bpos, IM_COL32(255, 255, 255, 160), 2.0f * zoom_);
    }

    // ---- nodes ----
    int connectExecTo = -1; // node whose exec-in completes the pending exec link
    for (int i = 0; i < (int)graph_.nodes.size(); ++i) {
        ScriptNode& n = graph_.nodes[i];
        const NodeDef* d = scriptNodeDef(n.type);
        ImVec2 tl = nodeTopLeft(n);
        float h = nodeHeight(n);
        ImVec2 br(tl.x + nodeW, tl.y + h);

        dl->AddRectFilled(tl, br, IM_COL32(34, 34, 43, 245), 6.0f * zoom_);
        dl->AddRectFilled(tl, ImVec2(br.x, tl.y + headH), headColor(d), 6.0f * zoom_,
                          ImDrawFlags_RoundCornersTop);
        char head[96];
        std::snprintf(head, sizeof(head), "%s%s%s", n.type.c_str(), n.p.empty() ? "" : "  ",
                      n.p.c_str());
        dl->AddText(font, fs * 0.9f, ImVec2(tl.x + 10 * zoom_, tl.y + 3.5f * zoom_),
                    IM_COL32(245, 246, 250, 255), head);

        if (d) {
            float y = tl.y + headH + 2.0f * zoom_;
            for (size_t e = 0; e < d->execOut.size(); ++e) { // exec-out rows (right)
                ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0,
                                                            d->execOut[e].c_str());
                dl->AddText(font, fs * 0.85f, ImVec2(br.x - ts.x - 12 * zoom_, y),
                            IM_COL32(220, 222, 230, 255), d->execOut[e].c_str());
                y += rowH;
            }
            for (size_t p = 0; p < d->dataIn.size(); ++p) { // data-in rows (left)
                char row[80];
                const DataIn& di = p < n.in.size() ? n.in[p] : DataIn{};
                if (di.fromNode.empty()) {
                    const Value& v = di.hasLiteral ? di.literal : d->dataIn[p].def;
                    std::snprintf(row, sizeof(row), "%s = %s", d->dataIn[p].name.c_str(),
                                  v.asS().c_str());
                } else {
                    std::snprintf(row, sizeof(row), "%s <-", d->dataIn[p].name.c_str());
                }
                dl->AddText(font, fs * 0.85f, ImVec2(tl.x + 12 * zoom_, y),
                            IM_COL32(190, 193, 202, 255), row);
                y += rowH;
            }
            for (size_t p = 0; p < d->dataOut.size(); ++p) { // data-out rows (right)
                ImVec2 ts = ImGui::GetFont()->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0,
                                                            d->dataOut[p].name.c_str());
                dl->AddText(font, fs * 0.85f, ImVec2(br.x - ts.x - 12 * zoom_, y),
                            (ImU32)pinTypeColor(d->dataOut[p].type), d->dataOut[p].name.c_str());
                y += rowH;
            }
        }

        if (selected_ == i)
            dl->AddRect(ImVec2(tl.x - 1, tl.y - 1), ImVec2(br.x + 1, br.y + 1),
                        IM_COL32(255, 255, 255, 200), 6.0f * zoom_, 0, 2.0f);

        // Exec-in pin (header left) when the node accepts execution.
        if (d && (!d->execIn.empty() || d->isEvent == false))
            if (!d->pure() && !d->isEvent && n.type != "Function")
                dl->AddCircleFilled(execInPos(n), 4.5f * zoom_, IM_COL32(235, 235, 245, 255));

        // Node body interaction.
        ImGui::SetCursorScreenPos(tl);
        ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##node", ImVec2(nodeW, h));
        if (ImGui::IsItemActivated()) {
            selected_ = i;
            if (linkNode_ >= 0 && linkIsExec_ && i != linkNode_) connectExecTo = i;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) &&
            linkNode_ < 0) {
            n.x += io.MouseDelta.x / zoom_;
            n.y += io.MouseDelta.y / zoom_;
            dirty_ = true;
        }
        if (ImGui::BeginPopupContextItem("node_ctx")) {
            selected_ = i;
            if (ImGui::MenuItem("Delete node")) {
                std::string dead = n.id;
                for (auto& other : graph_.nodes) {
                    for (auto& ex : other.execOut)
                        if (ex == dead) ex.clear();
                    for (auto& di : other.in)
                        if (di.fromNode == dead) di = DataIn{};
                }
                graph_.nodes.erase(graph_.nodes.begin() + i);
                selected_ = -1;
                dirty_ = true;
                ImGui::EndPopup();
                ImGui::PopID();
                dl->PopClipRect();
                return;
            }
            ImGui::EndPopup();
        }

        if (d) {
            // Exec-out pins.
            for (int pin = 0; pin < (int)d->execOut.size(); ++pin) {
                ImVec2 p = execOutPos(n, pin);
                dl->AddCircleFilled(p, 4.5f * zoom_, IM_COL32(235, 235, 245, 255));
                ImGui::SetCursorScreenPos(ImVec2(p.x - 7 * zoom_, p.y - 7 * zoom_));
                ImGui::PushID(pin + 1000);
                if (ImGui::InvisibleButton("##epin", ImVec2(14 * zoom_, 14 * zoom_))) {
                    if (pin < (int)n.execOut.size() && !n.execOut[pin].empty()) {
                        n.execOut[pin].clear();
                        dirty_ = true;
                    }
                    linkNode_ = i;
                    linkPin_ = pin;
                    linkIsExec_ = true;
                }
                if (ImGui::IsItemHovered())
                    dl->AddCircle(p, 6.5f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                ImGui::PopID();
            }
            // Data-in pins (complete a pending data link, or grab an existing one).
            for (int pin = 0; pin < (int)d->dataIn.size(); ++pin) {
                ImVec2 p = dataInPos(n, d, pin);
                dl->AddCircleFilled(p, 4.0f * zoom_, (ImU32)pinTypeColor(d->dataIn[pin].type));
                ImGui::SetCursorScreenPos(ImVec2(p.x - 7 * zoom_, p.y - 7 * zoom_));
                ImGui::PushID(pin + 2000);
                if (ImGui::InvisibleButton("##dpin", ImVec2(14 * zoom_, 14 * zoom_))) {
                    if (linkNode_ >= 0 && !linkIsExec_ && linkNode_ != i) {
                        if (pin < (int)n.in.size()) {
                            n.in[pin].fromNode = graph_.nodes[linkNode_].id;
                            n.in[pin].fromOut = linkPin_;
                            n.in[pin].hasLiteral = false;
                            dirty_ = true;
                        }
                        linkNode_ = -1;
                    } else if (pin < (int)n.in.size() && !n.in[pin].fromNode.empty()) {
                        // Grab the existing link to rewire it.
                        int src = graph_.indexOf(n.in[pin].fromNode);
                        int srcPin = n.in[pin].fromOut;
                        n.in[pin] = DataIn{};
                        dirty_ = true;
                        if (src >= 0) {
                            linkNode_ = src;
                            linkPin_ = srcPin;
                            linkIsExec_ = false;
                        }
                    }
                }
                if (ImGui::IsItemHovered())
                    dl->AddCircle(p, 6.0f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                ImGui::PopID();
            }
            // Data-out pins (start a data link).
            for (int pin = 0; pin < (int)d->dataOut.size(); ++pin) {
                ImVec2 p = dataOutPos(n, d, pin);
                dl->AddCircleFilled(p, 4.0f * zoom_, (ImU32)pinTypeColor(d->dataOut[pin].type));
                ImGui::SetCursorScreenPos(ImVec2(p.x - 7 * zoom_, p.y - 7 * zoom_));
                ImGui::PushID(pin + 3000);
                if (ImGui::InvisibleButton("##opin", ImVec2(14 * zoom_, 14 * zoom_))) {
                    linkNode_ = i;
                    linkPin_ = pin;
                    linkIsExec_ = false;
                }
                if (ImGui::IsItemHovered())
                    dl->AddCircle(p, 6.0f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
                ImGui::PopID();
            }
        }
        ImGui::PopID();
    }

    // Complete a pending EXEC link on node-body click.
    if (connectExecTo >= 0 && linkNode_ >= 0 && linkIsExec_) {
        ScriptNode& from = graph_.nodes[linkNode_];
        const NodeDef* toDef = scriptNodeDef(graph_.nodes[connectExecTo].type);
        bool accepts = toDef && !toDef->pure() && !toDef->isEvent &&
                       graph_.nodes[connectExecTo].type != "Function";
        if (accepts && linkPin_ < (int)from.execOut.size()) {
            from.execOut[linkPin_] = graph_.nodes[connectExecTo].id;
            dirty_ = true;
        }
        linkNode_ = -1;
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// sidebar
// ---------------------------------------------------------------------------
static bool editValue(const char* label, Value& v) {
    bool changed = false;
    ImGui::PushID(label);
    int t = (int)v.type - 1; // skip Exec
    const char* names[] = {"Float", "Vec3", "Bool", "String", "Entity"};
    ImGui::SetNextItemWidth(90);
    if (ImGui::Combo("##type", &t, names, 4)) { // Entity literals not editable
        v.type = (PinType)(t + 1);
        changed = true;
    }
    ImGui::SameLine();
    switch (v.type) {
    case PinType::Float: changed |= ImGui::DragFloat(label, &v.f, 0.05f); break;
    case PinType::Vec3: changed |= ImGui::DragFloat3(label, &v.v.x, 0.05f); break;
    case PinType::Bool: changed |= ImGui::Checkbox(label, &v.b); break;
    case PinType::String: {
        char buf[260];
        std::snprintf(buf, sizeof(buf), "%s", v.s.c_str());
        if (ImGui::InputText(label, buf, sizeof(buf))) { v.s = buf; changed = true; }
        break;
    }
    default: ImGui::TextDisabled("%s", label); break;
    }
    ImGui::PopID();
    return changed;
}

void ScriptGraphPanel::drawSidebar() {
    // ---- variables ----
    ImGui::SeparatorText("Variables");
    int killVar = -1;
    for (int i = 0; i < (int)graph_.variables.size(); ++i) {
        ScriptVariable& v = graph_.variables[i];
        ImGui::PushID(i);
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", v.name.c_str());
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputText("##vn", nb, sizeof(nb))) { v.name = nb; dirty_ = true; }
        ImGui::SameLine();
        if (editValue("##vv", v.value)) dirty_ = true;
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) killVar = i;
        ImGui::PopID();
    }
    if (killVar >= 0) {
        graph_.variables.erase(graph_.variables.begin() + killVar);
        dirty_ = true;
    }
    if (ImGui::SmallButton("+ Variable")) {
        ScriptVariable v;
        v.name = "var" + std::to_string(graph_.variables.size() + 1);
        v.value = Value::F(0);
        graph_.variables.push_back(std::move(v));
        dirty_ = true;
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Node");
    if (selected_ < 0 || selected_ >= (int)graph_.nodes.size()) {
        ImGui::TextDisabled("Select a node.");
        ImGui::Spacing();
        ImGui::TextWrapped("White pins carry execution; colored pins carry typed data. "
                           "Click an output pin, then an input pin (or a node body for exec) "
                           "to wire. Right-click the canvas to add nodes (searchable).");
        return;
    }
    ScriptNode& n = graph_.nodes[selected_];
    const NodeDef* d = scriptNodeDef(n.type);
    ImGui::Text("%s", n.type.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", d ? d->category.c_str() : "?");

    char idBuf[64];
    std::snprintf(idBuf, sizeof(idBuf), "%s", n.id.c_str());
    if (ImGui::InputText("Id", idBuf, sizeof(idBuf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        std::string newId = idBuf;
        if (!newId.empty() && graph_.indexOf(newId) < 0) {
            for (auto& other : graph_.nodes) {
                for (auto& ex : other.execOut)
                    if (ex == n.id) ex = newId;
                for (auto& di : other.in)
                    if (di.fromNode == n.id) di.fromNode = newId;
            }
            n.id = newId;
            dirty_ = true;
        }
    }

    if (d && d->pLabel) {
        std::string prev = n.p;
        // Variable / function names get a picker when we can enumerate them.
        if (n.type == "GetVar" || n.type == "SetVar") {
            if (ImGui::BeginCombo(d->pLabel, n.p.empty() ? "(pick)" : n.p.c_str())) {
                for (const auto& v : graph_.variables)
                    if (ImGui::Selectable(v.name.c_str(), v.name == n.p)) n.p = v.name;
                ImGui::EndCombo();
            }
        } else if (n.type == "Call") {
            if (ImGui::BeginCombo(d->pLabel, n.p.empty() ? "(pick)" : n.p.c_str())) {
                for (const auto& fn : graph_.nodes)
                    if (fn.type == "Function")
                        if (ImGui::Selectable(fn.p.c_str(), fn.p == n.p)) n.p = fn.p;
                ImGui::EndCombo();
            }
        } else {
            inputText(d->pLabel, n.p);
        }
        if (prev != n.p) dirty_ = true;
    }
    if (d && d->nLabel) {
        float prev = n.n;
        ImGui::DragFloat(d->nLabel, &n.n, 0.05f);
        if (prev != n.n) dirty_ = true;
    }

    // Literals for unconnected data inputs.
    if (d && !d->dataIn.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Inputs");
        for (int p = 0; p < (int)d->dataIn.size(); ++p) {
            if (p >= (int)n.in.size()) break;
            DataIn& di = n.in[p];
            ImGui::PushID(p);
            if (!di.fromNode.empty()) {
                ImGui::Text("%s <- %s", d->dataIn[p].name.c_str(), di.fromNode.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("x")) { di = DataIn{}; dirty_ = true; }
            } else {
                if (!di.hasLiteral) {
                    di.literal = d->dataIn[p].def;
                    di.literal.type = d->dataIn[p].type;
                }
                if (editValue(d->dataIn[p].name.c_str(), di.literal)) {
                    di.hasLiteral = true;
                    dirty_ = true;
                }
            }
            ImGui::PopID();
        }
    }

    // Exec-out wiring summary.
    if (d && !d->execOut.empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Exec outs");
        for (int p = 0; p < (int)n.execOut.size(); ++p) {
            const char* label = p < (int)d->execOut.size() ? d->execOut[p].c_str() : "then";
            ImGui::Text("%s -> %s", label, n.execOut[p].empty() ? "(none)" : n.execOut[p].c_str());
            if (!n.execOut[p].empty()) {
                ImGui::SameLine();
                ImGui::PushID(p + 500);
                if (ImGui::SmallButton("x")) { n.execOut[p].clear(); dirty_ = true; }
                ImGui::PopID();
            }
        }
    }
}

} // namespace ae
