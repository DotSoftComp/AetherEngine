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
        loaded_ = false;
        if (docPath.empty()) return;
        std::string rel = docPath;
        loaded_ = loadUIDocument(doc_, assets.resolvePath(rel));
        docPath = rel;
    }

    // Called by the app layer (game runtime / editor Play) each frame with the
    // game HUD's UI context and screen area.
    void draw(UI& ui, const Rect& area) {
        if (!loaded_) return;
        std::vector<std::string> clicked;
        World& w = world();
        drawUIDocument(ui, doc_.root, area,
                       [&w](const std::string& f) { return w.missions.flag(f); }, &clicked);
        for (const auto& id : clicked) w.postUIEvent(id);
    }

    UIDocument& document() { return doc_; }
    bool documentLoaded() const { return loaded_; }
    void reload(AssetLibrary& assets) { onDeserialized(assets); }

private:
    UIDocument doc_;
    bool loaded_ = false;
};

} // namespace ae
