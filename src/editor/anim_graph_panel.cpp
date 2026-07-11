// Aether Engine — animation state-machine editor (see anim_graph_panel.h).
#include "anim_graph_panel.h"
#include "../engine/world.h"
#include "../engine/entity.h"
#include "../engine/assets.h"
#include "../engine/components.h"
#include "../render/gltf.h"
#include "../core/log.h"
#include "../core/math3d.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ae {

bool AnimGraphPanel::createStarterGraph(const std::string& path) {
    AnimGraph g;
    g.start = "idle";
    g.parameters.push_back({"Speed", 0.0f});
    AnimState idle;
    idle.id = "idle";
    idle.clip = "Idle";
    idle.x = 80;
    idle.y = 120;
    AnimState walk;
    walk.id = "walk";
    walk.clip = "Walk";
    walk.x = 380;
    walk.y = 120;
    g.states.push_back(idle);
    g.states.push_back(walk);
    g.transitions.push_back({"idle", "walk", "Speed", ">", 0.2f, 0.25f});
    g.transitions.push_back({"walk", "idle", "Speed", "<=", 0.2f, 0.25f});
    return saveAnimGraph(g, path);
}

void AnimGraphPanel::open(const std::string& path, bool focus) {
    AnimGraph g;
    if (!loadAnimGraph(g, path)) return;
    graph_ = std::move(g);
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    selState_ = -1;
    selTransition_ = -1;
    if (focus) {
        visible = true;
        focusRequested_ = true;
    }
}

AnimatorComponent* AnimGraphPanel::liveAnimator(World* world, AssetLibrary* assets) {
    if (!world || !assets) return nullptr;
    for (const auto& e : world->entities())
        for (const auto& c : e->components())
            if (auto* a = dynamic_cast<AnimatorComponent*>(c.get()))
                if (!a->graphPath.empty() && assets->resolvePath(a->graphPath) == path_)
                    return a;
    return nullptr;
}

bool AnimGraphPanel::save(World* world, AssetLibrary* assets) {
    if (!saveAnimGraph(graph_, path_)) return false;
    dirty_ = false;
    if (world && assets)
        for (const auto& e : world->entities())
            for (const auto& c : e->components())
                if (auto* a = dynamic_cast<AnimatorComponent*>(c.get()))
                    if (!a->graphPath.empty() && assets->resolvePath(a->graphPath) == path_)
                        a->reload(*assets);
    return true;
}

void AnimGraphPanel::draw(World* world, AssetLibrary* assets) {
    if (!visible) return;
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    ImGui::SetNextWindowSize(ImVec2(980, 600), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Anim Graph", &visible)) {
        ImGui::End();
        return;
    }
    if (!loaded_) {
        ImGui::TextDisabled("No anim graph loaded.");
        ImGui::TextDisabled("Double-click a .json in assets/anim in the Content Browser,");
        ImGui::TextDisabled("or use Tools > Anim Graph Editor.");
        ImGui::End();
        return;
    }
    drawToolbar(world, assets);
    ImGui::Separator();

    const float sidebarW = 320.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas", ImVec2(avail.x - sidebarW - 8.0f, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas(world, assets);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sidebar");
    drawSidebar(world, assets);
    ImGui::EndChild();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        save(world, assets);
    ImGui::End();
}

void AnimGraphPanel::drawToolbar(World* world, AssetLibrary* assets) {
    if (ImGui::Button(dirty_ ? "Save*" : "Save")) save(world, assets);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) open(path_);
    ImGui::SameLine();
    if (ImGui::Button("+ State")) {
        AnimState s;
        for (int i = 1;; ++i) {
            char id[16];
            std::snprintf(id, sizeof(id), "state%d", i);
            if (graph_.stateIndex(id) < 0) { s.id = id; break; }
        }
        s.clip = "Idle";
        s.x = (220.0f - pan_.x) / zoom_;
        s.y = (140.0f - pan_.y) / zoom_;
        graph_.states.push_back(std::move(s));
        selState_ = (int)graph_.states.size() - 1;
        selTransition_ = -1;
        dirty_ = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %s  ·  %d states  ·  %d transitions", path_.c_str(),
                        (int)graph_.states.size(), (int)graph_.transitions.size());
}

void AnimGraphPanel::drawCanvas(World* world, AssetLibrary* assets) {
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
        zoom_ = clampf(zoom_ * (1.0f + io.MouseWheel * 0.10f), 0.4f, 2.0f);
        ImVec2 mouse(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        pan_.x = mouse.x - (mouse.x - pan_.x) * (zoom_ / old);
        pan_.y = mouse.y - (mouse.y - pan_.y) * (zoom_ / old);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) { selState_ = -1; selTransition_ = -1; }

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
    const float nodeW = 150.0f * zoom_, nodeH = 54.0f * zoom_;

    auto tl = [&](const AnimState& s) {
        return ImVec2(origin.x + pan_.x + s.x * zoom_, origin.y + pan_.y + s.y * zoom_);
    };
    auto centerOf = [&](const AnimState& s) {
        ImVec2 p = tl(s);
        return ImVec2(p.x + nodeW * 0.5f, p.y + nodeH * 0.5f);
    };

    // Live state highlight during Play.
    std::string liveState;
    if (AnimatorComponent* a = liveAnimator(world, assets)) liveState = a->currentState();

    // ---- transitions as arrows (offset when a reverse pair exists) ----
    for (int i = 0; i < (int)graph_.transitions.size(); ++i) {
        const AnimTransition& t = graph_.transitions[i];
        int fi = graph_.stateIndex(t.from), ti = graph_.stateIndex(t.to);
        if (fi < 0 || ti < 0) continue;
        ImVec2 a = centerOf(graph_.states[fi]);
        ImVec2 b = centerOf(graph_.states[ti]);
        ImVec2 d(b.x - a.x, b.y - a.y);
        float len = std::sqrt(d.x * d.x + d.y * d.y);
        if (len < 1.0f) continue;
        d.x /= len;
        d.y /= len;
        ImVec2 n(-d.y, d.x); // perpendicular offset so A->B and B->A don't overlap
        float off = 7.0f * zoom_;
        a = ImVec2(a.x + n.x * off, a.y + n.y * off);
        b = ImVec2(b.x + n.x * off, b.y + n.y * off);
        // Trim to node edges (approximate).
        float trim = 62.0f * zoom_;
        a = ImVec2(a.x + d.x * trim * 0.7f, a.y + d.y * trim * 0.42f);
        b = ImVec2(b.x - d.x * trim * 0.7f, b.y - d.y * trim * 0.42f);

        ImU32 col = selTransition_ == i ? IM_COL32(255, 255, 255, 255)
                                        : IM_COL32(150, 130, 240, 220);
        dl->AddLine(a, b, col, 2.0f * zoom_);
        ImVec2 tip = b;
        ImVec2 l(tip.x - d.x * 10 * zoom_ + n.x * 5 * zoom_,
                 tip.y - d.y * 10 * zoom_ + n.y * 5 * zoom_);
        ImVec2 r(tip.x - d.x * 10 * zoom_ - n.x * 5 * zoom_,
                 tip.y - d.y * 10 * zoom_ - n.y * 5 * zoom_);
        dl->AddTriangleFilled(tip, l, r, col);

        // Clickable midpoint (selects the transition).
        ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        dl->AddCircleFilled(mid, 4.5f * zoom_, col);
        ImGui::SetCursorScreenPos(ImVec2(mid.x - 8 * zoom_, mid.y - 8 * zoom_));
        ImGui::PushID(i + 5000);
        if (ImGui::InvisibleButton("##tr", ImVec2(16 * zoom_, 16 * zoom_))) {
            selTransition_ = i;
            selState_ = -1;
        }
        if (ImGui::IsItemHovered())
            dl->AddCircle(mid, 7.0f * zoom_, IM_COL32(255, 255, 255, 220), 0, 1.5f);
        ImGui::PopID();
    }

    // ---- states ----
    for (int i = 0; i < (int)graph_.states.size(); ++i) {
        AnimState& s = graph_.states[i];
        ImVec2 p = tl(s);
        ImVec2 br(p.x + nodeW, p.y + nodeH);
        bool isStart = graph_.start == s.id;
        bool isLive = !liveState.empty() && liveState == s.id;

        dl->AddRectFilled(p, br, IM_COL32(34, 34, 43, 245), 8.0f * zoom_);
        dl->AddRectFilled(p, ImVec2(br.x, p.y + 20 * zoom_),
                          isStart ? IM_COL32(80, 150, 60, 255) : IM_COL32(100, 90, 190, 255),
                          8.0f * zoom_, ImDrawFlags_RoundCornersTop);
        char head[64];
        std::snprintf(head, sizeof(head), "%s%s", s.id.c_str(), isStart ? "  [start]" : "");
        dl->AddText(font, fs * 0.9f, ImVec2(p.x + 8 * zoom_, p.y + 2.5f * zoom_),
                    IM_COL32(245, 246, 250, 255), head);
        char body[80];
        std::snprintf(body, sizeof(body), "%s  x%.2g%s", s.clip.c_str(), s.speed,
                      s.loop ? "" : "  (once)");
        dl->AddText(font, fs * 0.85f, ImVec2(p.x + 8 * zoom_, p.y + 26 * zoom_),
                    IM_COL32(195, 198, 208, 255), body);

        ImU32 outline = 0;
        float thick = 1.0f;
        if (selState_ == i) { outline = IM_COL32(255, 255, 255, 200); thick = 2.0f; }
        if (isLive) { outline = IM_COL32(190, 170, 255, 255); thick = 3.0f; }
        if (outline)
            dl->AddRect(ImVec2(p.x - 1, p.y - 1), ImVec2(br.x + 1, br.y + 1), outline,
                        8.0f * zoom_, 0, thick);

        ImGui::SetCursorScreenPos(p);
        ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##state", ImVec2(nodeW, nodeH));
        if (ImGui::IsItemActivated()) { selState_ = i; selTransition_ = -1; }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            s.x += io.MouseDelta.x / zoom_;
            s.y += io.MouseDelta.y / zoom_;
            dirty_ = true;
        }
        if (ImGui::BeginPopupContextItem("state_ctx")) {
            selState_ = i;
            selTransition_ = -1;
            if (ImGui::MenuItem("Set as start")) { graph_.start = s.id; dirty_ = true; }
            if (ImGui::BeginMenu("Add transition to")) {
                for (const auto& other : graph_.states) {
                    if (other.id == s.id) continue;
                    if (ImGui::MenuItem(other.id.c_str())) {
                        graph_.transitions.push_back(
                            {s.id, other.id, graph_.parameters.empty()
                                                 ? "Speed"
                                                 : graph_.parameters[0].name,
                             ">", 0.0f, 0.25f});
                        selTransition_ = (int)graph_.transitions.size() - 1;
                        selState_ = -1;
                        dirty_ = true;
                    }
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Delete state")) {
                std::string dead = s.id;
                ImGui::EndPopup();
                ImGui::PopID();
                for (int t = (int)graph_.transitions.size() - 1; t >= 0; --t)
                    if (graph_.transitions[t].from == dead || graph_.transitions[t].to == dead)
                        graph_.transitions.erase(graph_.transitions.begin() + t);
                graph_.states.erase(graph_.states.begin() + i);
                selState_ = -1;
                selTransition_ = -1;
                dirty_ = true;
                dl->PopClipRect();
                return;
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }

    dl->PopClipRect();
}

void AnimGraphPanel::drawSidebar(World* world, AssetLibrary* assets) {
    AnimatorComponent* live = liveAnimator(world, assets);

    ImGui::SeparatorText("Parameters");
    int killP = -1;
    for (int i = 0; i < (int)graph_.parameters.size(); ++i) {
        AnimParam& p = graph_.parameters[i];
        ImGui::PushID(i);
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", p.name.c_str());
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputText("##pn", nb, sizeof(nb))) { p.name = nb; dirty_ = true; }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::DragFloat("##pv", &p.value, 0.05f)) dirty_ = true;
        if (live) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1), "live %.2f", live->param(p.name));
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) killP = i;
        ImGui::PopID();
    }
    if (killP >= 0) { graph_.parameters.erase(graph_.parameters.begin() + killP); dirty_ = true; }
    if (ImGui::SmallButton("+ Parameter")) {
        graph_.parameters.push_back({"param" + std::to_string(graph_.parameters.size() + 1), 0});
        dirty_ = true;
    }
    if (live) {
        ImGui::TextDisabled("Playing: state '%s'", live->currentState().c_str());
    }

    ImGui::Spacing();
    if (selState_ >= 0 && selState_ < (int)graph_.states.size()) {
        AnimState& s = graph_.states[selState_];
        ImGui::SeparatorText("State");
        char idb[64];
        std::snprintf(idb, sizeof(idb), "%s", s.id.c_str());
        if (ImGui::InputText("Id", idb, sizeof(idb), ImGuiInputTextFlags_EnterReturnsTrue)) {
            std::string ni = idb;
            if (!ni.empty() && graph_.stateIndex(ni) < 0) {
                for (auto& t : graph_.transitions) {
                    if (t.from == s.id) t.from = ni;
                    if (t.to == s.id) t.to = ni;
                }
                if (graph_.start == s.id) graph_.start = ni;
                s.id = ni;
                dirty_ = true;
            }
        }
        // Clip combo populated from the live Animator's model when available.
        Model* model = nullptr;
        if (world)
            for (const auto& e : world->entities()) {
                if (auto* mc = e->getComponent<ModelComponent>())
                    if (mc->model && mc->model->clipCount() > 0) { model = mc->model; break; }
            }
        if (model) {
            if (ImGui::BeginCombo("Clip", s.clip.c_str())) {
                for (int c = 0; c < model->clipCount(); ++c)
                    if (ImGui::Selectable(model->clipName(c), s.clip == model->clipName(c))) {
                        s.clip = model->clipName(c);
                        dirty_ = true;
                    }
                ImGui::EndCombo();
            }
        } else {
            char cb[64];
            std::snprintf(cb, sizeof(cb), "%s", s.clip.c_str());
            if (ImGui::InputText("Clip", cb, sizeof(cb))) { s.clip = cb; dirty_ = true; }
        }
        if (ImGui::DragFloat("Speed", &s.speed, 0.01f, 0.0f, 10.0f)) dirty_ = true;
        bool loop = s.loop;
        if (ImGui::Checkbox("Loop", &loop)) { s.loop = loop; dirty_ = true; }
    } else if (selTransition_ >= 0 && selTransition_ < (int)graph_.transitions.size()) {
        AnimTransition& t = graph_.transitions[selTransition_];
        ImGui::SeparatorText("Transition");
        ImGui::Text("%s -> %s", t.from.c_str(), t.to.c_str());
        if (ImGui::BeginCombo("Parameter", t.param.c_str())) {
            for (const auto& p : graph_.parameters)
                if (ImGui::Selectable(p.name.c_str(), p.name == t.param)) {
                    t.param = p.name;
                    dirty_ = true;
                }
            ImGui::EndCombo();
        }
        const char* ops[] = {">", "<", ">=", "<=", "==", "!="};
        int cur = 0;
        for (int i = 0; i < 6; ++i)
            if (t.op == ops[i]) cur = i;
        if (ImGui::Combo("Op", &cur, ops, 6)) { t.op = ops[cur]; dirty_ = true; }
        if (ImGui::DragFloat("Value", &t.value, 0.05f)) dirty_ = true;
        if (ImGui::DragFloat("Blend (s)", &t.blend, 0.01f, 0.0f, 5.0f)) dirty_ = true;
        if (ImGui::Button("Delete transition")) {
            graph_.transitions.erase(graph_.transitions.begin() + selTransition_);
            selTransition_ = -1;
            dirty_ = true;
        }
    } else {
        ImGui::SeparatorText("Help");
        ImGui::TextWrapped("Drag states to arrange. Right-click a state to set the start "
                           "state, add a transition, or delete it. Click an arrow's midpoint "
                           "dot to edit its condition. Gameplay drives Parameters via "
                           "Animator::setParam or the SetAnimParam script node.");
    }
}

} // namespace ae
