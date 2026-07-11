// Aether Engine — node-graph editor for DialogueScene files: the Detroit-style
// flowchart, editable. Nodes (Line / Choice / QTE / End) are drawn on a
// pannable, zoomable canvas with bezier links between out-pins and nodes;
// drag pins to rewire branches, edit fields in the sidebar, and save straight
// back to the JSON the runtime plays. During Play the taken path lights up
// live (visited nodes + current node), like the game's own flowchart screen.
#pragma once
#include "../narrative/dialogue_scene.h"
#include "imgui.h"
#include <string>

namespace ae {

class World;

class DialogueGraphEditor {
public:
    bool visible = true;

    void open(const std::string& path, bool focus = true); // load a file (focus its tab)
    void draw(World* world);            // builds the "Dialogue Graph" window
    bool loaded() const { return loaded_; }
    const std::string& path() const { return path_; }

private:
    void drawToolbar(World* world);
    void drawCanvas(World* world);
    void drawSidebar();
    void addNode(NodeType type, float x, float y);
    void deleteNode(int idx);
    void renameNodeId(int idx, const std::string& newId);
    void autoLayout();
    int nodeIndex(const std::string& id) const;
    bool save(World* world); // world: live triggers reload the edited file

    // Out-pin -> target-string mapping (pin = option index for Choice, then
    // timeout; 0 = next for Line; 0/1 = success/fail for QTE).
    static std::string* pinTarget(DialogueNode& n, int pin);
    static int pinCount(const DialogueNode& n);

    DialogueScene scene_;
    std::string path_;
    bool loaded_ = false;
    bool dirty_ = false;
    bool focusRequested_ = false;

    ImVec2 pan_{80.0f, 60.0f};
    float zoom_ = 1.0f;
    int selected_ = -1;
    int linkFromNode_ = -1;
    int linkFromPin_ = -1;
    char idEdit_[64] = {};
    int idEditNode_ = -1;
};

} // namespace ae
