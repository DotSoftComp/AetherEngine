// Aether Engine — headless navmesh smoke test.
//
// No window/GL: builds a world (floor + a wall across the middle), bakes the
// Recast navmesh from its static colliders, and asserts (1) a path across the
// map exists and actually detours around the wall, (2) an unreachable target
// fails cleanly, and (3) a kinematic NavAgent walks the path to arrival via
// World::update. Exit code 0 = pass.
#include "engine/world.h"
#include "physics/physics_components.h"
#include "ai/nav_agent.h"
#include <cmath>
#include <cstdio>

using namespace ae;

// 24x24 m floor with a 12 m wall across the middle (a gap on +Z only).
static void buildArena(World& w) {
    Entity* g = w.spawn("Ground");
    auto* gc = g->addComponent<ColliderComponent>();
    gc->halfExtents = Vec3(12.0f, 0.1f, 12.0f); // top at y = 0.1

    Entity* wall = w.spawn("Wall");
    wall->transform.position = Vec3(0, 1.0f, -3.0f);
    auto* wc = wall->addComponent<ColliderComponent>();
    wc->halfExtents = Vec3(0.5f, 1.0f, 9.0f); // z spans -12..6: gap at z 6..12
}

static bool testPathAroundWall() {
    World w;
    buildArena(w);
    Input in;
    w.update(1.0f / 60.0f, 0.0f, in, true); // resolve world transforms

    if (!w.nav.bake(w)) {
        std::printf("[NavSmoke] bake FAILED\n");
        return false;
    }
    std::printf("[NavSmoke] baked: %d polys\n", w.nav.polyCount());

    std::vector<Vec3> path;
    if (!w.nav.findPath(Vec3(-6, 0.1f, -3), Vec3(6, 0.1f, -3), path)) {
        std::printf("[NavSmoke] findPath FAILED\n");
        return false;
    }
    float maxZ = -1e9f;
    for (const Vec3& p : path) maxZ = std::max(maxZ, p.z);
    // The wall reaches z = 6; a straight line has z = -3 throughout, so a real
    // detour must swing past the wall end (agent radius keeps it > ~6).
    bool detours = path.size() >= 3 && maxZ > 5.0f;
    std::printf("[NavSmoke] path: %d waypoints, maxZ=%.2f (wall end 6.0) -> %s\n",
                (int)path.size(), maxZ, detours ? "PASS" : "FAIL");
    return detours;
}

static bool testUnreachable() {
    World w;
    buildArena(w);
    Input in;
    w.update(1.0f / 60.0f, 0.0f, in, true);
    w.nav.bake(w);

    std::vector<Vec3> path;
    bool found = w.nav.findPath(Vec3(-6, 0.1f, -3), Vec3(0, 40.0f, 0), path, 1.0f);
    std::printf("[NavSmoke] unreachable target rejected -> %s\n", !found ? "PASS" : "FAIL");
    return !found;
}

static bool testAgentWalks() {
    World w;
    buildArena(w);
    Entity* npc = w.spawn("NPC");
    npc->transform.position = Vec3(-6, 0.1f, -3);
    auto* agent = npc->addComponent<NavAgentComponent>();
    agent->speed = 4.0f;

    Input in;
    const float dt = 1.0f / 60.0f;
    w.update(dt, 0.0f, in, true);
    agent->moveTo(Vec3(6, 0.1f, -3)); // auto-bakes on first update

    float walked = 0.0f;
    Vec3 prev = npc->transform.position;
    int frames = 0;
    for (; frames < 60 * 20 && !agent->arrived(); ++frames) {
        w.update(dt, frames * dt, in, true);
        walked += length(npc->transform.position - prev);
        prev = npc->transform.position;
    }
    Vec3 end = npc->transform.position;
    float goalDist = length(Vec3(end.x - 6, 0, end.z + 3));
    // Straight-line distance is 12; the detour around the wall must be longer.
    bool pass = agent->arrived() && goalDist < 0.6f && walked > 13.0f;
    std::printf("[NavSmoke] agent: arrived=%d in %d frames, dist-to-goal=%.2f, "
                "walked=%.1f m (straight 12.0) -> %s\n",
                agent->arrived(), frames, goalDist, walked, pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    bool ok = true;
    ok &= testPathAroundWall();
    ok &= testUnreachable();
    ok &= testAgentWalks();
    std::printf("[NavSmoke] %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
