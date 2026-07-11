// Aether Engine — the Details panel's reflected inspector. One PropertyVisitor
// that renders ImGui widgets for any component's reflect() — built-ins and
// game-module scripts get their editor UI with zero editor code. PropKind
// picks the widget (drags, sliders, color pickers, asset combos, entity
// pickers); see engine/reflect.h.
#pragma once
#include "../engine/reflect.h"
#include <functional>
#include <string>

namespace ae {

class AssetLibrary;
class World;
class Component;

class ImGuiInspectorVisitor : public PropertyVisitor {
public:
    ImGuiInspectorVisitor(AssetLibrary* assets, World* world) : assets_(assets), world_(world) {}

    // Wired by the editor: "Open in Dialogue Graph" on DialogueScenePath props.
    std::function<void(const std::string&)> openDialogueScene;
    // Wired by the editor: "Open in Script Graph" on ScriptGraphPath props.
    std::function<void(const std::string&)> openScriptGraph;
    // Wired by the editor: "Open in Material Graph" on MaterialGraphPath props.
    std::function<void(const std::string&)> openMaterialGraph;
    // Wired by the editor: "Open in UI Designer" on UIDocPath props.
    std::function<void(const std::string&)> openUIDesigner;
    // Wired by the editor: "Open in Anim Graph" on AnimGraphPath props.
    std::function<void(const std::string&)> openAnimGraph;

    // Call before reflecting each component (CameraDefaultFlag exclusivity).
    void setCurrent(Component* c) {
        current_ = c;
        assetsDirty_ = false;
    }
    // An asset-identity property (mesh/texture-set/model/dialogue path)
    // changed — the editor should call component->onDeserialized(assets).
    bool assetsDirty() const { return assetsDirty_; }

    void visit(const char* key, float& v, const PropMeta& m) override;
    void visit(const char* key, int& v, const PropMeta& m) override;
    void visit(const char* key, bool& v, const PropMeta& m) override;
    void visit(const char* key, std::string& v, const PropMeta& m) override;
    void visit(const char* key, Vec3& v, const PropMeta& m) override;
    void visit(const char* key, Vec4& v, const PropMeta& m) override;
    void visit(const char* guidKey, const char* nameKey, EntityRef& ref, std::string& legacyName,
               const PropMeta& m) override;
    void beginGroup(const char* key) override;
    void endGroup() override;

private:
    AssetLibrary* assets_ = nullptr;
    World* world_ = nullptr;
    Component* current_ = nullptr;
    bool assetsDirty_ = false;
};

} // namespace ae
