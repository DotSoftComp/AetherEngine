// Aether Engine — headless save-game test: snapshot a running world (entities,
// story flags, mission states, script variables), wreck everything, load the
// save, and assert full restoration. Exit 0 = pass.
#include "engine/world.h"
#include "engine/assets.h"
#include "engine/save_game.h"
#include "engine/component_registry.h"
#include "script/script_graph.h"
#include <cmath>
#include <cstdio>

using namespace ae;

static int fails = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) { std::printf("[SaveSmoke] FAIL: %s (line %d)\n", #cond, __LINE__); ++fails; } \
    } while (0)

int main() {
    registerBuiltinComponents();
    AssetLibrary assets; // no init: no GL needed for this path
    Input in;
    const char* file = "save_smoke.json";

    World world;
    Entity* hero = world.spawn("Hero");
    hero->transform.position = Vec3(1, 2, 3);
    auto* sg = hero->addComponent<ScriptGraphComponent>();
    sg->setVar("hp", Value::F(77));
    sg->setVar("checkpoint", Value::S("bridge"));
    world.spawn("Chest");
    world.missions.setFlag("gold", 42);
    Mission m;
    m.id = "quest";
    m.state = MissionState::Active;
    m.objectives.push_back({});
    m.objectives[0].state = ObjectiveState::Complete;
    world.missions.missions.push_back(m);

    CHECK(saveGame(world, assets, file));

    // Wreck the running state.
    hero->transform.position = Vec3(9, 9, 9);
    sg->setVar("hp", Value::F(1));
    world.destroy(world.find("Chest"));
    world.update(0.0f, 0.0f, in, false); // sweep the destroy
    world.missions.setFlag("gold", 0);
    world.missions.setFlag("junk", 1);
    world.missions.missions[0].state = MissionState::Failed;

    CHECK(loadGame(world, assets, file));

    Entity* hero2 = world.find("Hero");
    CHECK(hero2 != nullptr);
    CHECK(hero2 && std::fabs(hero2->transform.position.x - 1.0f) < 1e-4f &&
          std::fabs(hero2->transform.position.z - 3.0f) < 1e-4f);
    CHECK(world.find("Chest") != nullptr);
    CHECK(world.missions.flag("gold") == 42);
    CHECK(world.missions.flag("junk") == 0); // post-save state must not leak in
    CHECK(world.missions.missions[0].state == MissionState::Active);
    CHECK(world.missions.missions[0].objectives[0].state == ObjectiveState::Complete);
    auto* sg2 = hero2 ? hero2->getComponent<ScriptGraphComponent>() : nullptr;
    CHECK(sg2 != nullptr);
    CHECK(sg2 && std::fabs(sg2->getVar("hp").asF() - 77.0f) < 1e-4f);
    CHECK(sg2 && sg2->getVar("checkpoint").asS() == "bridge");

    std::remove(file);
    std::printf("[SaveSmoke] %s\n", fails == 0 ? "ALL PASS" : "FAIL");
    return fails == 0 ? 0 : 1;
}
