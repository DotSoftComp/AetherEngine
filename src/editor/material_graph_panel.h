// Aether Engine — node-canvas editor for MaterialGraph assets. Pure dataflow:
// typed pins (Float/Vec2/Vec3, colored) wire node outputs into inputs; the
// Output node holds the PBR result. Table-driven from materialNodeDefs(), so
// new node types appear automatically. Save regenerates GLSL, recompiles the
// cached shader in place, and refreshes components using the file.
#pragma once
#include "../render/material_graph.h"
#include "imgui.h"
#include <string>

namespace ae {

class World;
class AssetLibrary;

class MaterialGraphPanel {
public:
    bool visible = false;

    void open(const std::string& path, bool focus = true);
    void draw(World* world, AssetLibrary* assets);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    static bool createStarterGraph(const std::string& path);

private:
    void drawToolbar(World* world, AssetLibrary* assets);
    void drawCanvas();
    void drawSidebar();
    void addNodeMenu(float gx, float gy);
    void addNode(const std::string& type, float x, float y);
    bool save(World* world, AssetLibrary* assets);

    MaterialGraph graph_;
    std::string path_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;

    ImVec2 pan_{80.0f, 60.0f};
    float zoom_ = 1.0f;
    int selected_ = -1;
    int linkNode_ = -1; // pending link source (output pin)
    char paletteFilter_[48] = {};
};

} // namespace ae
