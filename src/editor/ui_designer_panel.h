// Aether Engine — WYSIWYG designer for UIDocument assets (retained game UI).
// Left: widget hierarchy. Center: a scaled virtual-screen preview — click to
// select, drag to move (writes the pixel offset). Right: properties with 3x3
// anchor/pivot presets, so HUDs stay resolution-independent. Save hot-reloads
// every UIDocument component using the file.
#pragma once
#include "../ui/ui_document.h"
#include "imgui.h"
#include <string>

namespace ae {

class World;
class AssetLibrary;

class UIDesignerPanel {
public:
    bool visible = false;

    void open(const std::string& path, bool focus = true);
    void draw(World* world, AssetLibrary* assets);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    static bool createStarterDoc(const std::string& path);

private:
    void drawToolbar(World* world, AssetLibrary* assets);
    void drawTree(UIWidget& w);
    void drawCanvas();
    void drawProps();
    bool save(World* world, AssetLibrary* assets);
    UIWidget* selected() { return selectedId_.empty() ? nullptr : doc_.find(selectedId_); }
    void addChild(const char* type);
    bool removeWidget(UIWidget& parent, const std::string& id);

    UIDocument doc_;
    std::string path_;
    std::string selectedId_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;
    int nextId_ = 1;
};

} // namespace ae
