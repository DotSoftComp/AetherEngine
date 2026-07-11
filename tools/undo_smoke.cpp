// Aether Engine — headless test for the undo/redo foundation.
//
// Undo is whole-world snapshots via serializeWorld/deserializeWorld. This drives
// that machinery directly (no editor/ImGui): snapshot a world, mutate it, then
// restore the snapshot and verify the mutation is gone — and that a redo-style
// re-apply brings it back. Exit 0 = pass.
#include "engine/world.h"
#include "engine/assets.h"
#include "engine/scene_io.h"
#include "engine/components.h"
#include "engine/component_registry.h"
#include <cstdio>
#include <string>

using namespace ae;

int main() {
    registerBuiltinComponents();
    // No AssetLibrary::init (that would touch GL) and no meshName, so the
    // MeshRenderer round-trips without needing a GL context or asset data.
    AssetLibrary assets;
    Input in;

    World world;
    Entity* a = world.spawn("Alpha");
    a->transform.position = Vec3(1, 2, 3);
    a->addComponent<MeshRenderer>();
    world.spawn("Beta");
    size_t before = world.entities().size();

    // Snapshot (this is exactly what recordUndo captures).
    std::string snap = serializeWorld(world, assets);

    // Mutate: delete one entity, move another, add a third.
    world.destroy(world.find("Beta"));
    world.update(0.0f, 0.0f, in, false); // process deferred destroy
    a->transform.position = Vec3(9, 9, 9);
    world.spawn("Gamma");
    size_t mutated = world.entities().size();
    std::string mutatedSnap = serializeWorld(world, assets);

    // Undo: restore the snapshot.
    deserializeWorld(world, assets, snap);
    size_t restored = world.entities().size();
    Entity* a2 = world.find("Alpha");
    bool posRestored = a2 && std::abs(a2->transform.position.x - 1.0f) < 1e-4f &&
                       std::abs(a2->transform.position.z - 3.0f) < 1e-4f;
    bool betaBack = world.find("Beta") != nullptr;
    bool gammaGone = world.find("Gamma") == nullptr;

    // Redo: re-apply the mutated state.
    deserializeWorld(world, assets, mutatedSnap);
    bool redoOk = world.entities().size() == mutated && world.find("Gamma") != nullptr &&
                  world.find("Beta") == nullptr;

    bool pass = before == 2 && mutated == 2 /*(-Beta +Gamma)*/ && restored == before &&
                posRestored && betaBack && gammaGone && redoOk;
    std::printf("[UndoSmoke] before=%zu mutated=%zu restored=%zu posRestored=%d betaBack=%d "
                "gammaGone=%d redoOk=%d -> %s\n",
                before, mutated, restored, posRestored, betaBack, gammaGone, redoOk,
                pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
