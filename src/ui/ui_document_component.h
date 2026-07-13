// Aether Engine — UIDocument component: attach a retained UI (assets/ui/*.json)
// to any entity and the game draws it over the 3D frame during Play. Label /
// ProgressBar bindings read the World's story flags live; Button clicks post
// World UI events, which the visual-scripting OnUIButton node consumes.
// Bridges engine <-> ui (same layering as narrative/dialogue_trigger.h).
#pragma once
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/reflect.h"
#include "../engine/assets.h"
#include "ui_document.h"

namespace ae {

class UIDocumentComponent : public Component {
public:
    std::string docPath; // project-relative assets/ui/*.json

    const char* typeName() const override { return "UIDocument"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("doc", docPath, {PropKind::UIDocPath, "UI document"});
    }
    void onDeserialized(AssetLibrary& assets) override {
        assets_ = &assets;
        loaded_ = false;
        if (docPath.empty()) return;
        std::string rel = docPath;
        loaded_ = loadUIDocument(doc_, assets.resolvePath(rel));
        docPath = rel;
    }

    // Called by the app layer (game runtime / editor Play) each frame with the
    // game HUD's UI context and screen area. Handles menu focus navigation
    // (keyboard + gamepad), Image sprites, and posts Button activations.
    void draw(UI& ui, const Rect& area) {
        if (!loaded_) return;
        World& w = world();
        std::vector<std::string> clicked;

        // ---- focus navigation: Down/Tab/DpadDown next, Up/DpadUp prev,
        //      Enter/Space/A activate. Focus is only claimed once nav is used
        //      (so mouse-only games are unaffected). ----
        std::vector<std::string> focusables;
        uiFocusables(doc_.root, focusables);
        const std::string* focusedId = nullptr;
        if (!focusables.empty()) {
            const Input& in = w.input();
            const GamepadState& pad = w.actions.gamepad();
            int n = (int)focusables.size();
            bool next = in.keys[0x28] || in.keys[0x09] || pad.buttons[(int)PadButton::DpadDown];
            bool prev = in.keys[0x26] || pad.buttons[(int)PadButton::DpadUp];
            bool act = in.keys[0x0D] || in.keys[0x20] || pad.buttons[(int)PadButton::A];
            if (next && !prevNext_) focusIndex_ = (focusIndex_ < 0) ? 0 : (focusIndex_ + 1) % n;
            if (prev && !prevPrev_) focusIndex_ = (focusIndex_ <= 0) ? n - 1 : focusIndex_ - 1;
            if (focusIndex_ >= n) focusIndex_ = n - 1;
            if (focusIndex_ >= 0) focusedId = &focusables[focusIndex_];
            if (act && !prevAct_ && focusedId) clicked.push_back(*focusedId);
            prevNext_ = next; prevPrev_ = prev; prevAct_ = act;
        }

        AssetLibrary* assets = assets_;
        drawUIDocument(
            ui, doc_.root, area, [&w](const std::string& f) { return w.missions.flag(f); },
            &clicked, [assets](const std::string& p) { return assets ? assets->uiImage(p) : 0u; },
            focusedId);
        for (const auto& id : clicked) w.postUIEvent(id);
    }

    UIDocument& document() { return doc_; }
    bool documentLoaded() const { return loaded_; }
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

private:
    UIDocument doc_;
    bool loaded_ = false;
    AssetLibrary* assets_ = nullptr;
    int focusIndex_ = -1;
    bool prevNext_ = false, prevPrev_ = false, prevAct_ = false;
};

} // namespace ae
