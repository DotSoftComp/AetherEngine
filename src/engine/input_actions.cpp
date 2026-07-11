// Aether Engine — action/axis input mapping (see input_actions.h).
#include "input_actions.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/math3d.h"
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace ae {

int keyCodeFromName(const std::string& name) {
    if (name.size() == 1) {
        char c = (char)std::toupper((unsigned char)name[0]);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    struct KV { const char* n; int v; };
    static const KV table[] = {
        {"SPACE", 0x20}, {"SHIFT", 0x10}, {"CTRL", 0x11}, {"ESC", 0x1B},
        {"ENTER", 0x0D}, {"TAB", 0x09},   {"UP", 0x26},   {"DOWN", 0x28},
        {"LEFT", 0x25},  {"RIGHT", 0x27}, {"LMB", -2},    {"RMB", -3},
        {"MMB", -4},
    };
    for (const KV& kv : table)
        if (_stricmp(kv.n, name.c_str()) == 0) return kv.v;
    return -1;
}

static bool sourceDown(const Input& in, const std::string& name) {
    int code = keyCodeFromName(name);
    if (code >= 0) return in.keys[code & 0xFF];
    if (code == -2) return in.mouseButtons[0];
    if (code == -3) return in.mouseButtons[1];
    if (code == -4) return in.mouseButtons[2];
    return false;
}

static float padAxisValue(const GamepadState& pad, const std::string& name) {
    if (_stricmp(name.c_str(), "LX") == 0) return pad.lx;
    if (_stricmp(name.c_str(), "LY") == 0) return pad.ly;
    if (_stricmp(name.c_str(), "RX") == 0) return pad.rx;
    if (_stricmp(name.c_str(), "RY") == 0) return pad.ry;
    if (_stricmp(name.c_str(), "LT") == 0) return pad.lt;
    if (_stricmp(name.c_str(), "RT") == 0) return pad.rt;
    return 0.0f;
}

void InputActions::setDefaults() {
    actions.clear();
    axes.clear();
    actions.push_back({"Jump", {"SPACE"}, {"A"}});
    actions.push_back({"Interact", {"E"}, {"X"}});
    actions.push_back({"Boost", {"B"}, {"B"}});
    actions.push_back({"Pause", {"ESC"}, {"START"}});
    axes.push_back({"MoveX", {"D"}, {"A"}, "LX", 1.0f});
    axes.push_back({"MoveY", {"W"}, {"S"}, "LY", 1.0f});
    axes.push_back({"LookX", {"RIGHT"}, {"LEFT"}, "RX", 1.0f});
    axes.push_back({"LookY", {"UP"}, {"DOWN"}, "RY", 1.0f});
}

void InputActions::loadOrDefaults(const std::string& path) {
    if (!load(path)) {
        setDefaults();
        AE_LOG("[Input] no input map at %s — using defaults", path.c_str());
    }
}

void InputActions::update(const Input& in, const GamepadState& pad) {
    pad_ = pad;
    for (auto& a : actions) {
        a.wasDown = a.down;
        bool d = false;
        for (const auto& k : a.keys)
            if (sourceDown(in, k)) { d = true; break; }
        if (!d && pad.connected)
            for (const auto& b : a.padButtons) {
                int idx = padButtonFromName(b.c_str());
                if (idx >= 0 && pad.buttons[idx]) { d = true; break; }
            }
        a.down = d;
    }
    for (auto& ax : axes) {
        float v = 0.0f;
        for (const auto& k : ax.posKeys)
            if (sourceDown(in, k)) { v += 1.0f; break; }
        for (const auto& k : ax.negKeys)
            if (sourceDown(in, k)) { v -= 1.0f; break; }
        if (v == 0.0f && pad.connected && !ax.padAxis.empty())
            v = padAxisValue(pad, ax.padAxis);
        ax.value = clampf(v * ax.scale, -1.0f, 1.0f);
    }
}

bool InputActions::down(const std::string& name) const {
    for (const auto& a : actions)
        if (a.name == name) return a.down;
    return false;
}
bool InputActions::pressed(const std::string& name) const {
    for (const auto& a : actions)
        if (a.name == name) return a.down && !a.wasDown;
    return false;
}
bool InputActions::released(const std::string& name) const {
    for (const auto& a : actions)
        if (a.name == name) return !a.down && a.wasDown;
    return false;
}
float InputActions::axis(const std::string& name) const {
    for (const auto& ax : axes)
        if (ax.name == name) return ax.value;
    return 0.0f;
}

// ---- (de)serialization -----------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}
static void writeList(std::ostringstream& o, const std::vector<std::string>& v) {
    o << "[";
    for (size_t i = 0; i < v.size(); ++i) o << (i ? ", " : "") << "\"" << esc(v[i]) << "\"";
    o << "]";
}
static std::vector<std::string> readList(const JsonValue* a) {
    std::vector<std::string> out;
    if (a)
        for (size_t i = 0; i < a->size(); ++i) out.push_back((*a)[i].str);
    return out;
}

bool InputActions::save(const std::string& path) const {
    std::ostringstream o;
    o << "{\n  \"inputMap\": 1,\n  \"actions\": [\n";
    for (size_t i = 0; i < actions.size(); ++i) {
        o << "    { \"name\": \"" << esc(actions[i].name) << "\", \"keys\": ";
        writeList(o, actions[i].keys);
        o << ", \"pad\": ";
        writeList(o, actions[i].padButtons);
        o << " }" << (i + 1 < actions.size() ? "," : "") << "\n";
    }
    o << "  ],\n  \"axes\": [\n";
    for (size_t i = 0; i < axes.size(); ++i) {
        o << "    { \"name\": \"" << esc(axes[i].name) << "\", \"pos\": ";
        writeList(o, axes[i].posKeys);
        o << ", \"neg\": ";
        writeList(o, axes[i].negKeys);
        o << ", \"padAxis\": \"" << esc(axes[i].padAxis) << "\", \"scale\": " << axes[i].scale
          << " }" << (i + 1 < axes.size() ? "," : "") << "\n";
    }
    o << "  ]\n}\n";

    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[Input] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[Input] saved %s", path.c_str());
    return f.good();
}

bool InputActions::load(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root) || !root.find("inputMap")) {
        AE_ERROR("[Input] malformed input map: %s", path.c_str());
        return false;
    }
    actions.clear();
    axes.clear();
    if (const JsonValue* as = root.find("actions")) {
        for (size_t i = 0; i < as->size(); ++i) {
            ActionBinding a;
            if (const std::string* n = (*as)[i].string("name")) a.name = *n;
            a.keys = readList((*as)[i].find("keys"));
            a.padButtons = readList((*as)[i].find("pad"));
            actions.push_back(std::move(a));
        }
    }
    if (const JsonValue* xs = root.find("axes")) {
        for (size_t i = 0; i < xs->size(); ++i) {
            AxisBinding x;
            if (const std::string* n = (*xs)[i].string("name")) x.name = *n;
            x.posKeys = readList((*xs)[i].find("pos"));
            x.negKeys = readList((*xs)[i].find("neg"));
            if (const std::string* p = (*xs)[i].string("padAxis")) x.padAxis = *p;
            x.scale = (float)(*xs)[i].num("scale", 1.0);
            axes.push_back(std::move(x));
        }
    }
    AE_LOG("[Input] loaded %d actions, %d axes from %s", (int)actions.size(), (int)axes.size(),
           path.c_str());
    return true;
}

} // namespace ae
