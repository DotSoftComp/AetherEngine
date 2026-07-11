// Aether Engine — Detroit-style mission/objective bookkeeping.
//
// Missions are data (JSON in assets/missions/) driven by world state:
//   - Reach objectives complete when the player entity gets near a target
//   - Flag objectives complete when a named story flag reaches a value
// Story flags are the shared branching state: dialogue nodes set them (see
// DialogueNode::setFlag), missions read them, and the editor's Mission panel
// shows them live during Play. The World owns one MissionSystem and ticks it
// alongside behaviors.
#pragma once
#include <map>
#include <string>
#include <vector>

namespace ae {

class World;

enum class ObjectiveType { Reach, Flag };
enum class ObjectiveState { Locked, Active, Complete, Failed };

struct Objective {
    std::string id;
    std::string text; // shown on the HUD
    ObjectiveType type = ObjectiveType::Reach;
    // Reach: player entity within `radius` of `targetEntity`.
    std::string targetEntity;
    float radius = 2.5f;
    // Flag: story flag `flag` equals `flagValue`.
    std::string flag;
    int flagValue = 1;
    bool optional = false;

    ObjectiveState state = ObjectiveState::Locked; // runtime only
};

enum class MissionState { NotStarted, Active, Complete, Failed };

struct Mission {
    std::string id;
    std::string name;
    std::string description;
    bool autoStart = false;
    bool sequential = true; // objectives unlock one at a time (else all at once)
    std::vector<Objective> objectives;

    MissionState state = MissionState::NotStarted; // runtime only
};

class MissionSystem {
public:
    std::vector<Mission> missions;
    std::string playerEntityName = "MainCamera";

    // ---- story flags ----
    void setFlag(const std::string& name, int value);
    int flag(const std::string& name) const;
    const std::map<std::string, int>& flags() const { return flags_; }

    // ---- runtime ----
    void startMission(const std::string& id);
    Mission* find(const std::string& id);
    void update(World& world, float dt); // ticks Active missions + toast ages
    void resetRuntime();                 // states/flags back to authored defaults

    // Completed-objective toasts for the HUD (newest last).
    struct Toast {
        std::string text;
        float age = 0.0f;
    };
    std::vector<Toast> toasts;

    // ---- persistence (assets/missions/missions.json) ----
    bool save(const std::string& path) const;
    bool load(const std::string& path);

private:
    void activateObjectives(Mission& m);
    std::map<std::string, int> flags_;
};

} // namespace ae
