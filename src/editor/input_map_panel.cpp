// Aether Engine — input map editor (see input_map_panel.h).
#include "input_map_panel.h"
#include "../engine/world.h"
#include "../engine/assets.h"
#include "imgui.h"
#include <cstdio>
#include <cstring>
#include <sstream>

namespace ae {

static std::string joinCSV(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) out += (i ? "," : "") + v[i];
    return out;
}
static std::vector<std::string> splitCSV(const char* s) {
    std::vector<std::string> out;
    std::string cur;
    for (const char* p = s;; ++p) {
        if (*p == ',' || *p == '\0') {
            // trim spaces
            size_t a = cur.find_first_not_of(' ');
            size_t b = cur.find_last_not_of(' ');
            if (a != std::string::npos) out.push_back(cur.substr(a, b - a + 1));
            cur.clear();
            if (*p == '\0') break;
        } else {
            cur += *p;
        }
    }
    return out;
}

static bool csvField(const char* id, std::vector<std::string>& list, float width) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s", joinCSV(list).c_str());
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputText(id, buf, sizeof(buf))) {
        list = splitCSV(buf);
        return true;
    }
    return false;
}

void InputMapPanel::draw(World* world, AssetLibrary* assets, const std::string& projectRoot) {
    if (!visible || !world) return;
    (void)assets;
    ImGui::SetNextWindowSize(ImVec2(660, 520), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Input Map", &visible)) {
        ImGui::End();
        return;
    }
    InputActions& map = world->actions;
    std::string path = projectRoot + "/assets/input.json";

    if (ImGui::Button(dirty_ ? "Save*" : "Save")) {
        if (map.save(path)) dirty_ = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { map.loadOrDefaults(path); dirty_ = false; }
    ImGui::SameLine();
    if (ImGui::Button("Restore defaults")) { map.setDefaults(); dirty_ = true; }
    ImGui::SameLine();
    ImGui::TextDisabled("| %s", path.c_str());

    ImGui::SeparatorText("Actions (keys / pad buttons, comma-separated)");
    int killA = -1;
    for (int i = 0; i < (int)map.actions.size(); ++i) {
        ActionBinding& a = map.actions[i];
        ImGui::PushID(i);
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", a.name.c_str());
        ImGui::SetNextItemWidth(110);
        if (ImGui::InputText("##an", nb, sizeof(nb))) { a.name = nb; dirty_ = true; }
        ImGui::SameLine();
        if (csvField("##ak", a.keys, 160)) dirty_ = true;
        ImGui::SameLine();
        if (csvField("##ap", a.padButtons, 130)) dirty_ = true;
        ImGui::SameLine();
        // Live state dot.
        ImGui::TextColored(a.down ? ImVec4(0.4f, 1, 0.4f, 1) : ImVec4(0.4f, 0.4f, 0.45f, 1),
                           a.down ? "DOWN" : "up");
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) killA = i;
        ImGui::PopID();
    }
    if (killA >= 0) { map.actions.erase(map.actions.begin() + killA); dirty_ = true; }
    if (ImGui::SmallButton("+ Action")) {
        map.actions.push_back({"NewAction", {}, {}});
        dirty_ = true;
    }

    ImGui::SeparatorText("Axes (pos keys / neg keys / pad axis)");
    int killX = -1;
    for (int i = 0; i < (int)map.axes.size(); ++i) {
        AxisBinding& x = map.axes[i];
        ImGui::PushID(i + 1000);
        char nb[64];
        std::snprintf(nb, sizeof(nb), "%s", x.name.c_str());
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputText("##xn", nb, sizeof(nb))) { x.name = nb; dirty_ = true; }
        ImGui::SameLine();
        if (csvField("##xp", x.posKeys, 100)) dirty_ = true;
        ImGui::SameLine();
        if (csvField("##xm", x.negKeys, 100)) dirty_ = true;
        ImGui::SameLine();
        char pa[16];
        std::snprintf(pa, sizeof(pa), "%s", x.padAxis.c_str());
        ImGui::SetNextItemWidth(50);
        if (ImGui::InputText("##xa", pa, sizeof(pa))) { x.padAxis = pa; dirty_ = true; }
        ImGui::SameLine();
        ImGui::Text("%+.2f", x.value);
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) killX = i;
        ImGui::PopID();
    }
    if (killX >= 0) { map.axes.erase(map.axes.begin() + killX); dirty_ = true; }
    if (ImGui::SmallButton("+ Axis")) {
        map.axes.push_back({"NewAxis", {}, {}, "", 1.0f});
        dirty_ = true;
    }

    ImGui::SeparatorText("Gamepad");
    const GamepadState& pad = map.gamepad();
    if (!pad.connected) {
        ImGui::TextDisabled("No controller connected (XInput slot 0).");
    } else {
        std::string held;
        for (int i = 0; i < (int)PadButton::Count; ++i)
            if (pad.buttons[i]) held += std::string(held.empty() ? "" : " ") +
                                        padButtonName((PadButton)i);
        ImGui::Text("Connected.  LS(%.2f, %.2f)  RS(%.2f, %.2f)  LT %.2f  RT %.2f", pad.lx,
                    pad.ly, pad.rx, pad.ry, pad.lt, pad.rt);
        ImGui::Text("Held: %s", held.empty() ? "(none)" : held.c_str());
    }
    ImGui::TextDisabled("Keys: A-Z 0-9 SPACE SHIFT CTRL ESC ENTER TAB UP DOWN LEFT RIGHT LMB "
                        "RMB MMB · Pad axes: LX LY RX RY LT RT");
    ImGui::End();
}

} // namespace ae
