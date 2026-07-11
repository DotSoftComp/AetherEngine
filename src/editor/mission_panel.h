// Aether Engine — Missions panel: author missions/objectives (saved to
// assets/missions/missions.json), and watch mission states + story flags
// update live during Play.
#pragma once
#include <string>

namespace ae {

class World;

class MissionPanel {
public:
    bool visible = true;

    // `missionsPath` is the absolute save path; `playing` switches on the
    // live-state / flags section.
    void draw(World& world, const std::string& missionsPath, bool playing);

private:
    int selected_ = -1;
};

} // namespace ae
