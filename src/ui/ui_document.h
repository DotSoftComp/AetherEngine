// Aether Engine — retained-mode game UI (the UMG-equivalent).
//
// A UIDocument is a tree of widgets (Panel / Label / Button / ProgressBar)
// saved as JSON in assets/ui/, authored in the editor's UI Designer, and drawn
// by the game over the 3D frame. Layout is resolution-independent: each widget
// places itself by an ANCHOR (0..1 point in its parent), a PIVOT (0..1 point
// of itself put at that anchor), a pixel OFFSET, and a pixel SIZE — the same
// model UMG/Unity use, so a HUD authored once works at any resolution.
//
// Dynamic data comes from story flags: Label text substitutes "{flag:name}"
// with the flag's value, and a ProgressBar fills from a flag against a max.
// Buttons report clicks by id; the UIDocument component posts them as World UI
// events, which the visual-scripting OnUIButton node receives — UI in the
// designer, behavior in the script graph, zero code.
//
// This layer is engine-free (pure widgets + the immediate-mode UI renderer);
// the World/component bridge lives in ui_document_component.h.
#pragma once
#include "ui.h"
#include <functional>
#include <string>
#include <vector>

namespace ae {

struct UIWidget {
    std::string id;
    std::string type = "Panel"; // Panel / Label / Button / ProgressBar / Image
    Vec2 anchor{0, 0};          // 0..1 point in the parent rect
    Vec2 pivot{0, 0};           // 0..1 point of this widget placed at the anchor
    Vec2 offset{0, 0};          // pixels from the anchor
    Vec2 size{160, 36};         // pixels
    bool visible = true;

    std::string text;           // Label/Button ({flag:name} substituted)
    Vec4 color{1, 1, 1, 1};     // text / bar-fill / Image tint color
    Vec4 bg{0, 0, 0, 0};        // background fill (alpha 0 = none)
    std::string bindFlag;       // ProgressBar: flag driving the fill
    float barMax = 1.0f;        // ProgressBar: flag value at 100%
    std::string image;          // Image: project-relative sprite path
    float fontScale = 1.0f;     // Label/Button: text size multiplier

    // Conditional visibility: when `showIfFlag` is set, this widget (and its
    // children) only draw while that story flag matches — != 0 by default, or
    // == showIfValue when that key is present. This is how a HUD shows the
    // keys you actually carry, swaps the weapon name, or reveals a death
    // overlay, without any script touching the document.
    std::string showIfFlag;
    bool hasShowIfValue = false;
    float showIfValue = 0.0f;

    std::vector<UIWidget> children;
};

struct UIDocument {
    UIWidget root; // type Panel, anchored to the whole screen
    UIDocument() {
        root.id = "root";
        root.type = "Panel";
    }
    UIWidget* find(const std::string& id);
};

bool loadUIDocument(UIDocument& doc, const std::string& path);
bool saveUIDocument(const UIDocument& doc, const std::string& path);

// Resolves one widget's rect inside its parent (shared by game + designer).
Rect uiWidgetRect(const UIWidget& w, const Rect& parent);

// True when a widget should draw this frame: `visible`, and (if it declares
// showIfFlag) its flag condition holds. A null flagValue shows everything,
// which is what the editor's designer preview wants.
bool uiWidgetShown(const UIWidget& w, const std::function<int(const std::string&)>& flagValue);

// Collects the ids of focusable (shown Button) widgets in tree order — the
// menu-navigation order for keyboard/gamepad focus.
void uiFocusables(const UIWidget& root, std::vector<std::string>& out,
                  const std::function<int(const std::string&)>& flagValue = {});

// Draws the tree over `area` and reports clicked Button ids. `flagValue`
// supplies story-flag lookups for bindings (may be null). `imageResolver`
// turns an Image widget's path into an rhi texture id (0 = none/missing).
// `focusedId` (menu nav) draws a focus ring on the matching Button.
void drawUIDocument(UI& ui, const UIWidget& root, const Rect& area,
                    const std::function<int(const std::string&)>& flagValue,
                    std::vector<std::string>* clickedOut,
                    const std::function<unsigned(const std::string&)>& imageResolver = {},
                    const std::string* focusedId = nullptr);

} // namespace ae
