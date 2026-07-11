// Aether Engine — editor for the project's input map (assets/input.json):
// named actions and axes with keyboard/mouse/gamepad bindings, edited as a
// table and saved back to the JSON the World loads. Live gamepad state is
// shown at the bottom so bindings can be sanity-checked with a controller.
#pragma once
#include <string>

namespace ae {

class World;
class AssetLibrary;

class InputMapPanel {
public:
    bool visible = false;

    // Edits world->actions in place; Save writes <project>/assets/input.json.
    void draw(World* world, AssetLibrary* assets, const std::string& projectRoot);

private:
    bool dirty_ = false;
};

} // namespace ae
