// Aether Engine — retained game-UI runtime (see ui_document.h).
#include "ui_document.h"
#include "../core/json.h"
#include "../core/log.h"
#include <cstdio>
#include <fstream>
#include <sstream>

namespace ae {

UIWidget* UIDocument::find(const std::string& id) {
    std::function<UIWidget*(UIWidget&)> rec = [&](UIWidget& w) -> UIWidget* {
        if (w.id == id) return &w;
        for (auto& c : w.children)
            if (UIWidget* r = rec(c)) return r;
        return nullptr;
    };
    return rec(root);
}

Rect uiWidgetRect(const UIWidget& w, const Rect& parent) {
    float ax = parent.x + parent.w * w.anchor.x + w.offset.x;
    float ay = parent.y + parent.h * w.anchor.y + w.offset.y;
    return {ax - w.size.x * w.pivot.x, ay - w.size.y * w.pivot.y, w.size.x, w.size.y};
}

// "{flag:name}" -> value of the flag (via the supplied lookup).
static std::string substitute(const std::string& text,
                              const std::function<int(const std::string&)>& flagValue) {
    if (!flagValue || text.find('{') == std::string::npos) return text;
    std::string out;
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, 6, "{flag:") == 0) {
            size_t close = text.find('}', i);
            if (close != std::string::npos) {
                std::string name = text.substr(i + 6, close - i - 6);
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%d", flagValue(name));
                out += buf;
                i = close + 1;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

void uiFocusables(const UIWidget& w, std::vector<std::string>& out) {
    if (!w.visible) return;
    if (w.type == "Button" && !w.id.empty()) out.push_back(w.id);
    for (const auto& c : w.children) uiFocusables(c, out);
}

static void drawWidget(UI& ui, const UIWidget& w, const Rect& parent,
                       const std::function<int(const std::string&)>& flagValue,
                       std::vector<std::string>* clickedOut,
                       const std::function<unsigned(const std::string&)>& imageResolver,
                       const std::string* focusedId, bool isRoot = false) {
    if (!w.visible) return;
    // The root IS the screen: it always spans the full area so child anchors
    // (0..1) reference the real resolution.
    Rect r = isRoot ? parent : uiWidgetRect(w, parent);

    auto col = [](const Vec4& c) { return rgba(c.x, c.y, c.z, c.w); };

    if (w.bg.w > 0.001f && w.type != "Button") ui.rectFill(r, col(w.bg));

    if (w.type == "Label") {
        std::string s = substitute(w.text, flagValue);
        ui.textCentered(r, s.c_str(), col(w.color), w.fontScale);
    } else if (w.type == "Button") {
        std::string s = substitute(w.text, flagValue);
        bool over = r.contains(ui.mouseX(), ui.mouseY());
        bool focused = focusedId && *focusedId == w.id;
        uint32_t base = w.bg.w > 0.001f ? col(w.bg) : rgba(0.16f, 0.15f, 0.22f, 0.92f);
        uint32_t hi = rgba(clampf(w.bg.x + 0.12f, 0, 1), clampf(w.bg.y + 0.12f, 0, 1),
                           clampf(w.bg.z + 0.16f, 0, 1), 0.96f);
        // Keyboard/gamepad focus reads distinctly from mouse hover: a violet
        // accent fill + a bright ring, so it's legible from across the room.
        if (focused) {
            ui.rectFill(r, rgba(0.34f, 0.27f, 0.58f, 0.96f));
            ui.rectLine(r, rgba(0.78f, 0.68f, 1.0f, 1.0f), 3.0f);
        } else {
            ui.rectFill(r, over ? hi : base);
        }
        ui.textCentered(r, s.c_str(), col(w.color), w.fontScale);
        if (over && ui.mousePressed() && clickedOut) clickedOut->push_back(w.id);
    } else if (w.type == "ProgressBar") {
        uint32_t track = w.bg.w > 0.001f ? col(w.bg) : rgba(0.10f, 0.10f, 0.14f, 0.85f);
        ui.rectFill(r, track);
        float v = 0.0f;
        if (flagValue && !w.bindFlag.empty() && w.barMax > 0.0001f)
            v = clampf((float)flagValue(w.bindFlag) / w.barMax, 0.0f, 1.0f);
        Rect fill = r.inset(2.0f);
        fill.w *= v;
        if (fill.w > 0.5f) ui.rectFill(fill, col(w.color));
        ui.rectLine(r, rgba(1, 1, 1, 0.25f));
    } else if (w.type == "Image") {
        unsigned tex = imageResolver ? imageResolver(w.image) : 0;
        if (tex) ui.image(r, tex, false);
        else ui.rectLine(r, rgba(1, 0, 1, 0.6f)); // magenta outline = missing sprite
    }
    // Panel: background only (drawn above), children below.

    for (const auto& c : w.children)
        drawWidget(ui, c, r, flagValue, clickedOut, imageResolver, focusedId);
}

void drawUIDocument(UI& ui, const UIWidget& root, const Rect& area,
                    const std::function<int(const std::string&)>& flagValue,
                    std::vector<std::string>* clickedOut,
                    const std::function<unsigned(const std::string&)>& imageResolver,
                    const std::string* focusedId) {
    drawWidget(ui, root, area, flagValue, clickedOut, imageResolver, focusedId, /*isRoot=*/true);
}

// ---- (de)serialization ----------------------------------------------------------
static std::string esc(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

static void writeWidget(std::ostringstream& o, const UIWidget& w, int depth) {
    std::string ind(depth * 2, ' ');
    o << ind << "{ \"id\": \"" << esc(w.id) << "\", \"type\": \"" << esc(w.type) << "\",\n";
    o << ind << "  \"anchor\": [" << w.anchor.x << ", " << w.anchor.y << "], \"pivot\": ["
      << w.pivot.x << ", " << w.pivot.y << "], \"offset\": [" << w.offset.x << ", "
      << w.offset.y << "], \"size\": [" << w.size.x << ", " << w.size.y << "]";
    if (!w.visible) o << ", \"visible\": false";
    if (!w.text.empty()) o << ", \"text\": \"" << esc(w.text) << "\"";
    o << ",\n" << ind << "  \"color\": [" << w.color.x << ", " << w.color.y << ", " << w.color.z
      << ", " << w.color.w << "], \"bg\": [" << w.bg.x << ", " << w.bg.y << ", " << w.bg.z
      << ", " << w.bg.w << "]";
    if (!w.bindFlag.empty()) o << ", \"bindFlag\": \"" << esc(w.bindFlag) << "\"";
    if (w.barMax != 1.0f) o << ", \"barMax\": " << w.barMax;
    if (!w.image.empty()) o << ", \"image\": \"" << esc(w.image) << "\"";
    if (w.fontScale != 1.0f) o << ", \"fontScale\": " << w.fontScale;
    if (!w.children.empty()) {
        o << ",\n" << ind << "  \"children\": [\n";
        for (size_t i = 0; i < w.children.size(); ++i) {
            writeWidget(o, w.children[i], depth + 2);
            o << (i + 1 < w.children.size() ? "," : "") << "\n";
        }
        o << ind << "  ]";
    }
    o << " }";
}

bool saveUIDocument(const UIDocument& doc, const std::string& path) {
    std::ostringstream o;
    o << "{\n  \"uiDocument\": 1,\n  \"root\":\n";
    writeWidget(o, doc.root, 1);
    o << "\n}\n";
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        AE_ERROR("[UI] cannot write %s", path.c_str());
        return false;
    }
    std::string text = o.str();
    f.write(text.data(), (std::streamsize)text.size());
    AE_LOG("[UI] saved %s", path.c_str());
    return f.good();
}

static Vec2 readVec2(const JsonValue* a, Vec2 def) {
    if (!a || a->size() < 2) return def;
    return Vec2((float)(*a)[0].number, (float)(*a)[1].number);
}
static Vec4 readVec4(const JsonValue* a, Vec4 def) {
    if (!a || a->size() < 4) return def;
    return Vec4((float)(*a)[0].number, (float)(*a)[1].number, (float)(*a)[2].number,
                (float)(*a)[3].number);
}

static void readWidget(const JsonValue& j, UIWidget& w) {
    if (const std::string* s = j.string("id")) w.id = *s;
    if (const std::string* s = j.string("type")) w.type = *s;
    w.anchor = readVec2(j.find("anchor"), w.anchor);
    w.pivot = readVec2(j.find("pivot"), w.pivot);
    w.offset = readVec2(j.find("offset"), w.offset);
    w.size = readVec2(j.find("size"), w.size);
    w.visible = j.flag("visible", true);
    if (const std::string* s = j.string("text")) w.text = *s;
    w.color = readVec4(j.find("color"), w.color);
    w.bg = readVec4(j.find("bg"), w.bg);
    if (const std::string* s = j.string("bindFlag")) w.bindFlag = *s;
    w.barMax = (float)j.num("barMax", 1.0);
    if (const std::string* s = j.string("image")) w.image = *s;
    w.fontScale = (float)j.num("fontScale", 1.0);
    if (const JsonValue* kids = j.find("children")) {
        for (size_t i = 0; i < kids->size(); ++i) {
            UIWidget c;
            readWidget((*kids)[i], c);
            w.children.push_back(std::move(c));
        }
    }
}

bool loadUIDocument(UIDocument& doc, const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_ERROR("[UI] cannot open %s", path.c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);

    JsonValue rootJ;
    if (!jsonParse(text.c_str(), text.size(), rootJ) || !rootJ.find("root")) {
        AE_ERROR("[UI] malformed document: %s", path.c_str());
        return false;
    }
    doc = UIDocument{};
    doc.root.children.clear();
    readWidget(*rootJ.find("root"), doc.root);
    return true;
}

} // namespace ae
