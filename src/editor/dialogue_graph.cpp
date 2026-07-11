#include "dialogue_graph.h"
#include "../engine/world.h"
#include "../narrative/dialogue_trigger.h"
#include "../narrative/dialogue_player.h"
#include "../core/log.h"
#include "imgui_internal.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <queue>
#include <set>

namespace ae {

// ---------------------------------------------------------------------------
// small helpers
// ---------------------------------------------------------------------------
namespace {

const char* typeName(NodeType t) {
    switch (t) {
    case NodeType::Line: return "LINE";
    case NodeType::Choice: return "CHOICE";
    case NodeType::Qte: return "QTE";
    default: return "END";
    }
}

ImU32 typeColor(NodeType t) {
    switch (t) {
    case NodeType::Line: return IM_COL32(62, 105, 170, 255);
    case NodeType::Choice: return IM_COL32(112, 84, 200, 255);
    case NodeType::Qte: return IM_COL32(196, 120, 42, 255);
    default: return IM_COL32(150, 52, 58, 255);
    }
}

int parseKeyToken(const std::string& s) {
    if (s == "SPACE") return VK_SPACE;
    if (s == "ENTER") return VK_RETURN;
    if (s == "SHIFT") return VK_SHIFT;
    if (s == "CTRL") return VK_CONTROL;
    if (s == "TAB") return VK_TAB;
    if (s == "UP") return VK_UP;
    if (s == "DOWN") return VK_DOWN;
    if (s == "LEFT") return VK_LEFT;
    if (s == "RIGHT") return VK_RIGHT;
    if (s.size() == 1) return std::toupper((unsigned char)s[0]);
    return 0;
}

std::string keyTokenName(int vk) {
    switch (vk) {
    case VK_SPACE: return "SPACE";
    case VK_RETURN: return "ENTER";
    case VK_SHIFT: return "SHIFT";
    case VK_CONTROL: return "CTRL";
    case VK_TAB: return "TAB";
    case VK_UP: return "UP";
    case VK_DOWN: return "DOWN";
    case VK_LEFT: return "LEFT";
    case VK_RIGHT: return "RIGHT";
    default:
        if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9'))
            return std::string(1, (char)vk);
        return "?";
    }
}

std::string keysToString(const std::vector<int>& keys) {
    std::string out;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i) out += " ";
        out += keyTokenName(keys[i]);
    }
    return out;
}

std::vector<int> stringToKeys(const std::string& s) {
    std::vector<int> out;
    std::string tok;
    for (size_t i = 0; i <= s.size(); ++i) {
        char c = i < s.size() ? s[i] : ' ';
        if (c == ' ' || c == ',' || c == ';') {
            if (!tok.empty()) {
                std::string up = tok;
                for (char& ch : up) ch = (char)std::toupper((unsigned char)ch);
                int vk = parseKeyToken(up);
                if (vk) out.push_back(vk);
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    return out;
}

// Outgoing branch targets of a node (read-only mirror of pinTarget).
std::vector<const std::string*> outTargets(const DialogueNode& n) {
    std::vector<const std::string*> out;
    switch (n.type) {
    case NodeType::Line: out.push_back(&n.next); break;
    case NodeType::Choice:
        for (const auto& o : n.options) out.push_back(&o.target);
        out.push_back(&n.timeoutTarget);
        break;
    case NodeType::Qte:
        out.push_back(&n.successTarget);
        out.push_back(&n.failTarget);
        break;
    case NodeType::End: break;
    }
    return out;
}

void inputStdString(const char* label, std::string& value, float width = -1.0f) {
    char buf[512];
    std::snprintf(buf, sizeof(buf), "%s", value.c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(label, buf, sizeof(buf))) value = buf;
}

} // namespace

// ---------------------------------------------------------------------------
// pins
// ---------------------------------------------------------------------------
std::string* DialogueGraphEditor::pinTarget(DialogueNode& n, int pin) {
    switch (n.type) {
    case NodeType::Line: return pin == 0 ? &n.next : nullptr;
    case NodeType::Choice:
        if (pin >= 0 && pin < (int)n.options.size()) return &n.options[pin].target;
        if (pin == (int)n.options.size()) return &n.timeoutTarget;
        return nullptr;
    case NodeType::Qte:
        if (pin == 0) return &n.successTarget;
        if (pin == 1) return &n.failTarget;
        return nullptr;
    default: return nullptr;
    }
}

int DialogueGraphEditor::pinCount(const DialogueNode& n) {
    switch (n.type) {
    case NodeType::Line: return 1;
    case NodeType::Choice: return (int)n.options.size() + 1; // + timeout
    case NodeType::Qte: return 2;
    default: return 0;
    }
}

// ---------------------------------------------------------------------------
// file ops
// ---------------------------------------------------------------------------
void DialogueGraphEditor::open(const std::string& path, bool focus) {
    DialogueScene s;
    if (!loadDialogueScene(s, path)) {
        AE_ERROR("[DialogueGraph] failed to load %s", path.c_str());
        return;
    }
    scene_ = std::move(s);
    path_ = path;
    loaded_ = true;
    dirty_ = false;
    selected_ = -1;
    linkFromNode_ = linkFromPin_ = -1;
    visible = true;
    focusRequested_ = focus;

    bool anyPos = false;
    for (const auto& n : scene_.nodes)
        if (n.edX != 0.0f || n.edY != 0.0f) { anyPos = true; break; }
    if (!anyPos) autoLayout();
    AE_LOG("[DialogueGraph] opened %s (%d nodes)", path.c_str(), (int)scene_.nodes.size());
}

bool DialogueGraphEditor::save(World* world) {
    if (!loaded_) return false;
    if (!saveDialogueScene(scene_, path_)) {
        AE_ERROR("[DialogueGraph] save failed: %s", path_.c_str());
        return false;
    }
    dirty_ = false;
    AE_LOG("[DialogueGraph] saved %s", path_.c_str());

    // Hot-reload any trigger in the world that plays this file.
    if (world) {
        for (const auto& e : world->entities()) {
            if (auto* trig = e->getComponent<DialogueTriggerComponent>()) {
                std::string a = trig->scenePath, b = path_;
                for (char& c : a) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
                for (char& c : b) c = c == '\\' ? '/' : (char)std::tolower((unsigned char)c);
                if (!a.empty() && b.size() >= a.size() &&
                    b.compare(b.size() - a.size(), a.size(), a) == 0) {
                    std::string keep = trig->scenePath;
                    trig->loadFromFile(path_);
                    trig->scenePath = keep;
                }
            }
        }
    }
    return true;
}

int DialogueGraphEditor::nodeIndex(const std::string& id) const {
    for (size_t i = 0; i < scene_.nodes.size(); ++i)
        if (scene_.nodes[i].id == id) return (int)i;
    return -1;
}

void DialogueGraphEditor::renameNodeId(int idx, const std::string& newId) {
    if (idx < 0 || idx >= (int)scene_.nodes.size() || newId.empty()) return;
    std::string old = scene_.nodes[idx].id;
    if (old == newId || nodeIndex(newId) >= 0) return; // keep ids unique
    scene_.nodes[idx].id = newId;
    if (scene_.startNode == old) scene_.startNode = newId;
    for (auto& n : scene_.nodes) {
        if (n.next == old) n.next = newId;
        if (n.timeoutTarget == old) n.timeoutTarget = newId;
        if (n.successTarget == old) n.successTarget = newId;
        if (n.failTarget == old) n.failTarget = newId;
        for (auto& o : n.options)
            if (o.target == old) o.target = newId;
    }
    dirty_ = true;
}

void DialogueGraphEditor::deleteNode(int idx) {
    if (idx < 0 || idx >= (int)scene_.nodes.size()) return;
    std::string id = scene_.nodes[idx].id;
    scene_.nodes.erase(scene_.nodes.begin() + idx);
    for (auto& n : scene_.nodes) {
        if (n.next == id) n.next.clear();
        if (n.timeoutTarget == id) n.timeoutTarget.clear();
        if (n.successTarget == id) n.successTarget.clear();
        if (n.failTarget == id) n.failTarget.clear();
        for (auto& o : n.options)
            if (o.target == id) o.target.clear();
    }
    if (scene_.startNode == id) scene_.startNode.clear();
    selected_ = -1;
    dirty_ = true;
}

void DialogueGraphEditor::addNode(NodeType type, float x, float y) {
    DialogueNode n;
    n.type = type;
    const char* base = type == NodeType::Line     ? "line"
                       : type == NodeType::Choice ? "choice"
                       : type == NodeType::Qte    ? "qte"
                                                  : "end";
    for (int i = 1;; ++i) {
        char id[48];
        std::snprintf(id, sizeof(id), "%s_%d", base, i);
        if (nodeIndex(id) < 0) {
            n.id = id;
            break;
        }
    }
    if (type == NodeType::Choice) n.options = {{"Option A", ""}, {"Option B", ""}};
    if (type == NodeType::Qte) n.qteKeys = {'F'};
    n.edX = x;
    n.edY = y;
    scene_.nodes.push_back(std::move(n));
    selected_ = (int)scene_.nodes.size() - 1;
    if (scene_.startNode.empty()) scene_.startNode = scene_.nodes.back().id;
    dirty_ = true;
}

void DialogueGraphEditor::autoLayout() {
    // BFS layering from the start node; unreachable nodes go in a last column.
    std::map<std::string, int> level;
    std::queue<std::string> q;
    if (nodeIndex(scene_.startNode) >= 0) {
        level[scene_.startNode] = 0;
        q.push(scene_.startNode);
    }
    int maxLevel = 0;
    while (!q.empty()) {
        std::string id = q.front();
        q.pop();
        int idx = nodeIndex(id);
        if (idx < 0) continue;
        for (const std::string* t : outTargets(scene_.nodes[idx])) {
            if (t->empty() || level.count(*t) || nodeIndex(*t) < 0) continue;
            level[*t] = level[id] + 1;
            maxLevel = std::max(maxLevel, level[*t]);
            q.push(*t);
        }
    }
    std::map<int, int> rowInLevel;
    for (auto& n : scene_.nodes) {
        int lv = level.count(n.id) ? level[n.id] : maxLevel + 1;
        int row = rowInLevel[lv]++;
        n.edX = 40.0f + lv * 270.0f;
        n.edY = 40.0f + row * 170.0f;
    }
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
void DialogueGraphEditor::draw(World* world) {
    if (focusRequested_) {
        ImGui::SetNextWindowFocus();
        focusRequested_ = false;
    }
    char title[300];
    std::snprintf(title, sizeof(title), "Dialogue Graph%s###DialogueGraph",
                  dirty_ ? " *" : "");
    if (!ImGui::Begin(title, &visible)) {
        ImGui::End();
        return;
    }

    if (!loaded_) {
        ImGui::TextDisabled("No dialogue scene loaded.");
        ImGui::TextDisabled("Double-click a .json in assets/dialogue in the Content Browser,");
        ImGui::TextDisabled("or use Tools > Dialogue Graph Editor.");
        ImGui::End();
        return;
    }

    drawToolbar(world);
    ImGui::Separator();

    float sidebarW = 300.0f;
    ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::BeginChild("##canvas", ImVec2(avail.x - sidebarW - 8.0f, 0), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawCanvas(world);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##sidebar");
    drawSidebar();
    ImGui::EndChild();

    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) &&
        ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S))
        save(world);

    ImGui::End();
}

void DialogueGraphEditor::drawToolbar(World* world) {
    if (ImGui::Button(dirty_ ? "Save*" : "Save")) save(world);
    ImGui::SameLine();
    if (ImGui::Button("Reload")) open(path_);
    ImGui::SameLine();
    if (ImGui::Button("Auto-layout")) {
        autoLayout();
        dirty_ = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Node")) ImGui::OpenPopup("add_node_toolbar");
    if (ImGui::BeginPopup("add_node_toolbar")) {
        float x = (200.0f - pan_.x) / zoom_, y = (120.0f - pan_.y) / zoom_;
        if (ImGui::MenuItem("Line")) addNode(NodeType::Line, x, y);
        if (ImGui::MenuItem("Choice")) addNode(NodeType::Choice, x, y);
        if (ImGui::MenuItem("QTE")) addNode(NodeType::Qte, x, y);
        if (ImGui::MenuItem("End")) addNode(NodeType::End, x, y);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| %s  ·  %d nodes  ·  zoom %.0f%%", path_.c_str(),
                        (int)scene_.nodes.size(), zoom_ * 100.0f);
}

// ---------------------------------------------------------------------------
// canvas
// ---------------------------------------------------------------------------
void DialogueGraphEditor::drawCanvas(World* world) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 40 || size.y < 40) return;

    // Background interaction layer. AllowOverlap is essential: the node/pin
    // buttons are submitted later in the frame, and without it this button
    // grabs ActiveId the instant the mouse goes down — before the nodes even
    // exist that frame — eating every node click.
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##bg", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    ImGuiIO& io = ImGui::GetIO();

    // Pan: middle-drag anywhere, or left-drag on empty space.
    if (ImGui::IsItemActive() && (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f) ||
                                  ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))) {
        pan_.x += io.MouseDelta.x;
        pan_.y += io.MouseDelta.y;
    }
    // Zoom around the mouse (anywhere over the canvas, nodes included).
    if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f) {
        float old = zoom_;
        zoom_ = clampf(zoom_ * (1.0f + io.MouseWheel * 0.10f), 0.35f, 2.0f);
        ImVec2 mouse(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
        pan_.x = mouse.x - (mouse.x - pan_.x) * (zoom_ / old);
        pan_.y = mouse.y - (mouse.y - pan_.y) * (zoom_ / old);
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) selected_ = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && linkFromNode_ >= 0) {
        linkFromNode_ = linkFromPin_ = -1; // cancel pending link
    }

    // Canvas context menu.
    if (ImGui::BeginPopupContextItem("canvas_ctx")) {
        ImVec2 mp = ImGui::GetMousePosOnOpeningCurrentPopup();
        float gx = (mp.x - origin.x - pan_.x) / zoom_;
        float gy = (mp.y - origin.y - pan_.y) / zoom_;
        ImGui::TextDisabled("Add node");
        ImGui::Separator();
        if (ImGui::MenuItem("Line")) addNode(NodeType::Line, gx, gy);
        if (ImGui::MenuItem("Choice")) addNode(NodeType::Choice, gx, gy);
        if (ImGui::MenuItem("QTE")) addNode(NodeType::Qte, gx, gy);
        if (ImGui::MenuItem("End")) addNode(NodeType::End, gx, gy);
        ImGui::Separator();
        if (ImGui::MenuItem("Auto-layout")) {
            autoLayout();
            dirty_ = true;
        }
        ImGui::EndPopup();
    }

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    // Grid.
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
    const float nodeW = 195.0f * zoom_;
    const float headH = 24.0f * zoom_;
    const float rowH = 19.0f * zoom_;

    // Live playthrough state (Detroit flowchart highlight).
    std::set<std::string> visited;
    std::string currentId;
    if (world) {
        if (DialoguePlayer* p = world->activeDialogue()) {
            if (p->scene().name == scene_.name) {
                for (const auto& v : p->visitedNodes()) visited.insert(v);
                if (p->currentNode()) currentId = p->currentNode()->id;
            }
        }
    }

    // Geometry helpers shared by links + nodes.
    auto nodeTopLeft = [&](const DialogueNode& n) {
        return ImVec2(origin.x + pan_.x + n.edX * zoom_, origin.y + pan_.y + n.edY * zoom_);
    };
    auto nodeHeight = [&](const DialogueNode& n) {
        int rows = 1; // summary row
        if (n.type == NodeType::Choice) rows = (int)n.options.size() + 1;
        if (n.type == NodeType::Qte) rows = 2;
        if (n.type == NodeType::End) rows = 1;
        return headH + rows * rowH + 8.0f * zoom_;
    };
    auto inPinPos = [&](const DialogueNode& n) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x, tl.y + headH * 0.5f);
    };
    auto outPinPos = [&](const DialogueNode& n, int pin) {
        ImVec2 tl = nodeTopLeft(n);
        return ImVec2(tl.x + nodeW, tl.y + headH + rowH * (pin + 0.5f));
    };
    auto pinColor = [&](const DialogueNode& n, int pin) -> ImU32 {
        if (n.type == NodeType::Qte)
            return pin == 0 ? IM_COL32(90, 200, 120, 255) : IM_COL32(225, 90, 95, 255);
        if (n.type == NodeType::Choice && pin == (int)n.options.size())
            return IM_COL32(225, 160, 70, 255); // timeout
        return IM_COL32(150, 130, 240, 255);
    };

    // Links underneath nodes.
    for (auto& n : scene_.nodes) {
        for (int pin = 0; pin < pinCount(n); ++pin) {
            const std::string& target = *pinTarget(n, pin);
            if (target.empty()) continue;
            int ti = nodeIndex(target);
            ImVec2 a = outPinPos(n, pin);
            if (ti < 0) {
                dl->AddLine(a, ImVec2(a.x + 26 * zoom_, a.y), IM_COL32(230, 70, 70, 200),
                            2.0f * zoom_);
                continue;
            }
            ImVec2 b = inPinPos(scene_.nodes[ti]);
            float bend = std::max(40.0f, std::fabs(b.x - a.x) * 0.5f) * zoom_;
            bool onPath = visited.count(n.id) && visited.count(target);
            ImU32 col = onPath ? IM_COL32(190, 170, 255, 255) : pinColor(n, pin);
            dl->AddBezierCubic(a, ImVec2(a.x + bend, a.y), ImVec2(b.x - bend, b.y), b, col,
                               (onPath ? 3.0f : 2.0f) * zoom_);
        }
    }

    // Pending link follows the mouse.
    if (linkFromNode_ >= 0 && linkFromNode_ < (int)scene_.nodes.size()) {
        ImVec2 a = outPinPos(scene_.nodes[linkFromNode_], linkFromPin_);
        ImVec2 b = io.MousePos;
        dl->AddBezierCubic(a, ImVec2(a.x + 60 * zoom_, a.y), ImVec2(b.x - 60 * zoom_, b.y), b,
                           IM_COL32(255, 255, 255, 160), 2.0f * zoom_);
    }

    // Nodes.
    int pendingConnectTo = -1;
    for (int i = 0; i < (int)scene_.nodes.size(); ++i) {
        DialogueNode& n = scene_.nodes[i];
        ImVec2 tl = nodeTopLeft(n);
        float h = nodeHeight(n);
        ImVec2 br(tl.x + nodeW, tl.y + h);

        // Body + header.
        bool isStart = scene_.startNode == n.id;
        bool isCurrent = currentId == n.id;
        bool wasVisited = visited.count(n.id) != 0;
        dl->AddRectFilled(tl, br, IM_COL32(34, 34, 43, 245), 6.0f * zoom_);
        dl->AddRectFilled(tl, ImVec2(br.x, tl.y + headH), typeColor(n.type), 6.0f * zoom_,
                          ImDrawFlags_RoundCornersTop);
        char head[96];
        std::snprintf(head, sizeof(head), "%s  %s%s", typeName(n.type), n.id.c_str(),
                      isStart ? "  [start]" : "");
        dl->AddText(font, fs * 0.92f, ImVec2(tl.x + 8 * zoom_, tl.y + 4 * zoom_),
                    IM_COL32(245, 246, 250, 255), head);

        // Rows.
        float y = tl.y + headH + 2.0f * zoom_;
        auto row = [&](const char* text, ImU32 col) {
            dl->AddText(font, fs * 0.88f, ImVec2(tl.x + 8 * zoom_, y), col, text);
            y += rowH;
        };
        char buf[160];
        switch (n.type) {
        case NodeType::Line:
            std::snprintf(buf, sizeof(buf), "%s%s\"%.28s\"", n.speaker.c_str(),
                          n.speaker.empty() ? "" : ": ", n.text.c_str());
            row(buf, IM_COL32(200, 203, 212, 255));
            break;
        case NodeType::Choice:
            for (size_t o = 0; o < n.options.size(); ++o) {
                std::snprintf(buf, sizeof(buf), "%d. %.24s", (int)o + 1,
                              n.options[o].text.c_str());
                row(buf, IM_COL32(200, 203, 212, 255));
            }
            std::snprintf(buf, sizeof(buf), "timeout (%.1fs)", n.timeLimit);
            row(buf, IM_COL32(225, 160, 70, 255));
            break;
        case NodeType::Qte:
            std::snprintf(buf, sizeof(buf), "success  [%s]", keysToString(n.qteKeys).c_str());
            row(buf, IM_COL32(120, 210, 145, 255));
            row("fail", IM_COL32(230, 120, 125, 255));
            break;
        case NodeType::End:
            row(n.setFlag.empty() ? "end of scene"
                                  : (std::string("sets ") + n.setFlag).c_str(),
                IM_COL32(160, 163, 172, 255));
            break;
        }

        // Selection / play-state outline.
        ImU32 outline = 0;
        float thick = 1.0f;
        if (wasVisited) { outline = IM_COL32(150, 130, 240, 160); thick = 1.5f; }
        if (selected_ == i) { outline = IM_COL32(255, 255, 255, 200); thick = 2.0f; }
        if (isCurrent) { outline = IM_COL32(200, 180, 255, 255); thick = 3.0f; }
        if (outline)
            dl->AddRect(ImVec2(tl.x - 1, tl.y - 1), ImVec2(br.x + 1, br.y + 1), outline,
                        6.0f * zoom_, 0, thick);

        // Node interaction (on top of the background button). AllowOverlap so
        // the pin buttons submitted right after can take clicks on the edge.
        ImGui::SetCursorScreenPos(tl);
        ImGui::PushID(i);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##node", ImVec2(nodeW, h));
        if (ImGui::IsItemActivated()) {
            selected_ = i;
            if (linkFromNode_ >= 0) pendingConnectTo = i;
        }
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f) &&
            linkFromNode_ < 0) {
            n.edX += io.MouseDelta.x / zoom_;
            n.edY += io.MouseDelta.y / zoom_;
            dirty_ = true;
        }
        if (ImGui::BeginPopupContextItem("node_ctx")) {
            selected_ = i;
            if (ImGui::MenuItem("Set as start node")) {
                scene_.startNode = n.id;
                dirty_ = true;
            }
            if (ImGui::MenuItem("Delete node")) {
                ImGui::EndPopup();
                ImGui::PopID();
                deleteNode(i);
                dl->PopClipRect();
                return; // indices shifted; redraw next frame
            }
            ImGui::EndPopup();
        }

        // In pin.
        ImVec2 ip = inPinPos(n);
        dl->AddCircleFilled(ip, 5.0f * zoom_, IM_COL32(210, 212, 220, 255));
        // Out pins.
        for (int pin = 0; pin < pinCount(n); ++pin) {
            ImVec2 op = outPinPos(n, pin);
            float r = 5.0f * zoom_;
            ImGui::SetCursorScreenPos(ImVec2(op.x - 8, op.y - 8));
            char pid[16];
            std::snprintf(pid, sizeof(pid), "##pin%d", pin);
            ImGui::InvisibleButton(pid, ImVec2(16, 16));
            bool pinHover = ImGui::IsItemHovered();
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                linkFromNode_ = i;
                linkFromPin_ = pin;
                pinTarget(n, pin)->clear(); // grabbing re-wires an existing link
                dirty_ = true;
            }
            dl->AddCircleFilled(op, pinHover ? r * 1.5f : r, pinColor(n, pin));
        }
        ImGui::PopID();
    }

    // Complete a pending link.
    if (pendingConnectTo >= 0 && linkFromNode_ >= 0 && linkFromNode_ != pendingConnectTo) {
        if (std::string* t = pinTarget(scene_.nodes[linkFromNode_], linkFromPin_)) {
            *t = scene_.nodes[pendingConnectTo].id;
            dirty_ = true;
        }
        linkFromNode_ = linkFromPin_ = -1;
    } else if (pendingConnectTo == linkFromNode_ && pendingConnectTo >= 0) {
        linkFromNode_ = linkFromPin_ = -1; // clicked the source node: cancel
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------------------
// sidebar
// ---------------------------------------------------------------------------
void DialogueGraphEditor::drawSidebar() {
    ImGui::SeparatorText("Scene");
    inputStdString("Name", scene_.name, 180);
    if (ImGui::BeginCombo("Start node", scene_.startNode.c_str())) {
        for (const auto& n : scene_.nodes)
            if (ImGui::Selectable(n.id.c_str(), n.id == scene_.startNode)) {
                scene_.startNode = n.id;
                dirty_ = true;
            }
        ImGui::EndCombo();
    }

    if (selected_ < 0 || selected_ >= (int)scene_.nodes.size()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Select a node to edit it.");
        ImGui::TextDisabled("Drag from an out-pin onto a node to");
        ImGui::TextDisabled("connect a branch. Right-click the canvas");
        ImGui::TextDisabled("to add nodes; wheel zooms, MMB pans.");
        return;
    }
    DialogueNode& n = scene_.nodes[selected_];

    ImGui::SeparatorText("Node");
    if (idEditNode_ != selected_) {
        std::snprintf(idEdit_, sizeof(idEdit_), "%s", n.id.c_str());
        idEditNode_ = selected_;
    }
    ImGui::SetNextItemWidth(160);
    ImGui::InputText("##nodeid", idEdit_, sizeof(idEdit_));
    ImGui::SameLine();
    if (ImGui::SmallButton("Rename")) renameNodeId(selected_, idEdit_);

    int type = (int)n.type;
    if (ImGui::Combo("Type", &type, "Line\0Choice\0QTE\0End\0")) {
        n.type = (NodeType)type;
        if (n.type == NodeType::Choice && n.options.empty())
            n.options = {{"Option A", ""}, {"Option B", ""}};
        if (n.type == NodeType::Qte && n.qteKeys.empty()) n.qteKeys = {'F'};
        dirty_ = true;
    }

    ImGui::SeparatorText("Camera");
    inputStdString("Camera", n.cameraName, 160);
    ImGui::SetNextItemWidth(120);
    ImGui::DragFloat("Blend (s)", &n.cameraBlend, 0.02f, 0.0f, 5.0f, "%.2f");

    ImGui::SeparatorText("Story flag");
    inputStdString("Set flag", n.setFlag, 160);
    if (!n.setFlag.empty()) {
        ImGui::SetNextItemWidth(120);
        ImGui::DragInt("Value", &n.setFlagValue, 0.1f);
    }

    switch (n.type) {
    case NodeType::Line: {
        ImGui::SeparatorText("Line");
        inputStdString("Speaker", n.speaker, 160);
        char text[512];
        std::snprintf(text, sizeof(text), "%s", n.text.c_str());
        if (ImGui::InputTextMultiline("Text", text, sizeof(text), ImVec2(-1, 60))) {
            n.text = text;
            dirty_ = true;
        }
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Duration", &n.duration, 0.05f, 0.2f, 30.0f, "%.1f s")) dirty_ = true;
        ImGui::TextDisabled("next -> %s", n.next.empty() ? "(unconnected)" : n.next.c_str());
        break;
    }
    case NodeType::Choice: {
        ImGui::SeparatorText("Choice");
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Time limit", &n.timeLimit, 0.05f, 0.0f, 60.0f, "%.1f s"))
            dirty_ = true;
        ImGui::TextDisabled("(0 = untimed)");
        int removeAt = -1;
        for (int i = 0; i < (int)n.options.size(); ++i) {
            ImGui::PushID(i);
            char buf[160];
            std::snprintf(buf, sizeof(buf), "%s", n.options[i].text.c_str());
            ImGui::SetNextItemWidth(-56);
            if (ImGui::InputText("##opt", buf, sizeof(buf))) {
                n.options[i].text = buf;
                dirty_ = true;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X")) removeAt = i;
            ImGui::SameLine();
            ImGui::TextDisabled("-> %s",
                                n.options[i].target.empty() ? "?" : n.options[i].target.c_str());
            ImGui::PopID();
        }
        if (removeAt >= 0) {
            n.options.erase(n.options.begin() + removeAt);
            dirty_ = true;
        }
        if (ImGui::SmallButton("+ Add option") && n.options.size() < 6) {
            n.options.push_back({"New option", ""});
            dirty_ = true;
        }
        ImGui::TextDisabled("timeout -> %s",
                            n.timeoutTarget.empty() ? "(end scene)" : n.timeoutTarget.c_str());
        break;
    }
    case NodeType::Qte: {
        ImGui::SeparatorText("Quick-Time Event");
        int qt = (int)n.qteType;
        if (ImGui::Combo("QTE type", &qt, "Tap\0Hold\0Mash\0Sequence\0")) {
            n.qteType = (QteType)qt;
            dirty_ = true;
        }
        char keys[128];
        std::snprintf(keys, sizeof(keys), "%s", keysToString(n.qteKeys).c_str());
        if (ImGui::InputText("Keys", keys, sizeof(keys))) {
            std::vector<int> parsed = stringToKeys(keys);
            if (!parsed.empty()) n.qteKeys = parsed;
            dirty_ = true;
        }
        ImGui::TextDisabled("letters/digits, UP DOWN LEFT RIGHT SPACE");
        ImGui::SetNextItemWidth(120);
        if (ImGui::DragFloat("Duration", &n.qteDuration, 0.02f, 0.2f, 10.0f, "%.2f s"))
            dirty_ = true;
        if (n.qteType == QteType::Hold) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragFloat("Hold time", &n.qteHoldSeconds, 0.02f, 0.1f, 5.0f, "%.2f s"))
                dirty_ = true;
        }
        if (n.qteType == QteType::Mash) {
            ImGui::SetNextItemWidth(120);
            if (ImGui::DragInt("Mash count", &n.qteMashCount, 0.2f, 2, 40)) dirty_ = true;
        }
        ImGui::TextDisabled("success -> %s",
                            n.successTarget.empty() ? "?" : n.successTarget.c_str());
        ImGui::TextDisabled("fail -> %s", n.failTarget.empty() ? "?" : n.failTarget.c_str());
        break;
    }
    case NodeType::End:
        ImGui::SeparatorText("End");
        ImGui::TextDisabled("Scene finishes here. Use 'Set flag' above");
        ImGui::TextDisabled("to report the outcome to missions.");
        break;
    }
}

} // namespace ae
