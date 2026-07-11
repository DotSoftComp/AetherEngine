// Aether Engine — editor for AnimGraph assets: a state-machine canvas (drag
// state boxes, arrows show transitions; the active state of a live Animator
// glows during Play), a parameters list with live values, and per-state /
// per-transition properties. Save hot-reloads every Animator running the file.
#pragma once
#include "../engine/anim_graph.h"
#include "imgui.h"
#include <string>

namespace ae {

class World;
class AssetLibrary;

class AnimGraphPanel {
public:
    bool visible = false;

    void open(const std::string& path, bool focus = true);
    void draw(World* world, AssetLibrary* assets);
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

    static bool createStarterGraph(const std::string& path);

private:
    void drawToolbar(World* world, AssetLibrary* assets);
    void drawCanvas(World* world, AssetLibrary* assets);
    void drawSidebar(World* world, AssetLibrary* assets);
    bool save(World* world, AssetLibrary* assets);
    AnimatorComponent* liveAnimator(World* world, AssetLibrary* assets);

    AnimGraph graph_;
    std::string path_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;

    ImVec2 pan_{80.0f, 60.0f};
    float zoom_ = 1.0f;
    int selState_ = -1;
    int selTransition_ = -1;
};

} // namespace ae
