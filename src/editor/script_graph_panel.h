// Aether Engine — node-canvas editor for ScriptGraph assets (visual scripting v2).
// Blueprint-style: white exec links drive control flow; colored, typed data
// links (Float/Vec3/Bool/String/Entity) feed values between pins. The canvas,
// pins, and sidebar are all driven by the node registry (scriptNodeDefs()), so
// new node types appear here with zero editor changes. Save writes the JSON
// back and hot-reloads every live ScriptGraph component running the file.
#pragma once
#include "../script/script_graph.h"
#include "imgui.h"
#include <string>

namespace ae {

class World;
class AssetLibrary;

class ScriptGraphPanel {
public:
    // Hidden until asked for (Tools/View menu, or double-clicking a script in
    // the Content Browser); fresh layouts dock it next to the Dialogue Graph.
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

    ScriptGraph graph_;
    std::string path_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;

    ImVec2 pan_{80.0f, 60.0f};
    float zoom_ = 1.0f;
    int selected_ = -1;

    // Pending link: from an output pin (exec or data) toward an input.
    int linkNode_ = -1;
    int linkPin_ = 0;
    bool linkIsExec_ = false;
    char paletteFilter_[48] = {};
};

} // namespace ae
