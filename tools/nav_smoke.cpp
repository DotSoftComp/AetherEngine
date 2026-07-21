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
#include "ai/nav_mesh.h"
#include "ai/behavior_tree.h"
#include <algorithm>
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

// Open floor, no wall: a straight crossing is short. Drop a dynamic obstacle
// dead center, tick the tile cache, and the re-planned path must detour (and
// removing it must restore the short path) — all without a rebake.
static bool testDynamicObstacle() {
    World w;
    Entity* g = w.spawn("Ground");
    g->addComponent<ColliderComponent>()->halfExtents = Vec3(12.0f, 0.1f, 12.0f);
    Input in;
    const float dt = 1.0f / 60.0f;
    w.update(dt, 0.0f, in, true);
    if (!w.nav.bake(w)) { std::printf("[NavSmoke] obstacle: bake FAILED\n"); return false; }

    std::vector<Vec3> before;
    w.nav.findPath(Vec3(-8, 0.1f, 0), Vec3(8, 0.1f, 0), before);
    float lenBefore = 0.0f;
    for (size_t i = 1; i < before.size(); ++i) lenBefore += length(before[i] - before[i - 1]);

    NavObstacle ob = w.nav.addObstacle(Vec3(0, 0.1f, 0), 2.5f, 2.0f);
    for (int i = 0; i < 12; ++i) w.nav.update(dt, w); // apply the carve

    std::vector<Vec3> during;
    w.nav.findPath(Vec3(-8, 0.1f, 0), Vec3(8, 0.1f, 0), during);
    float lenDuring = 0.0f;
    for (size_t i = 1; i < during.size(); ++i) lenDuring += length(during[i] - during[i - 1]);
    float maxAbsZ = 0.0f;
    for (const Vec3& p : during) maxAbsZ = std::max(maxAbsZ, std::fabs(p.z));

    w.nav.removeObstacle(ob);
    for (int i = 0; i < 12; ++i) w.nav.update(dt, w); // restore
    std::vector<Vec3> after;
    w.nav.findPath(Vec3(-8, 0.1f, 0), Vec3(8, 0.1f, 0), after);
    float lenAfter = 0.0f;
    for (size_t i = 1; i < after.size(); ++i) lenAfter += length(after[i] - after[i - 1]);

    // Detour swings off the straight line (z != 0) and is longer; removal
    // restores a near-straight path.
    bool pass = maxAbsZ > 1.0f && lenDuring > lenBefore + 0.5f && lenAfter < lenBefore + 0.3f;
    std::printf("[NavSmoke] obstacle: len %.1f -> %.1f (detour z=%.1f) -> %.1f restored -> %s\n",
                lenBefore, lenDuring, maxAbsZ, lenAfter, pass ? "PASS" : "FAIL");
    return pass;
}

// Two crowd agents swap places across an open floor. With avoidance on they
// must pass around each other — never occupying the same spot — and both
// still reach their goals.
static bool testCrowdAvoidance() {
    World w;
    Entity* g = w.spawn("Ground");
    g->addComponent<ColliderComponent>()->halfExtents = Vec3(12.0f, 0.1f, 12.0f);

    Entity* a = w.spawn("A");
    a->transform.position = Vec3(-6, 0.1f, 0);
    auto* aa = a->addComponent<NavAgentComponent>();
    aa->speed = 3.0f; aa->avoidance = true;
    Entity* b = w.spawn("B");
    b->transform.position = Vec3(6, 0.1f, 0);
    auto* ba = b->addComponent<NavAgentComponent>();
    ba->speed = 3.0f; ba->avoidance = true;

    Input in;
    const float dt = 1.0f / 60.0f;
    w.update(dt, 0.0f, in, true);
    aa->moveTo(Vec3(6, 0.1f, 0));
    ba->moveTo(Vec3(-6, 0.1f, 0));

    float minSep = 1e9f, maxLat = 0.0f;
    int frames = 0;
    for (; frames < 60 * 15 && (aa->isMoving() || ba->isMoving()); ++frames) {
        w.update(dt, frames * dt, in, true);
        Vec3 pa = a->transform.position, pb = b->transform.position;
        minSep = std::min(minSep, length(pa - pb));
        maxLat = std::max(maxLat, std::max(std::fabs(pa.z), std::fabs(pb.z)));
    }
    float aGoal = length(Vec3(a->transform.position.x - 6, 0, a->transform.position.z));
    float bGoal = length(Vec3(b->transform.position.x + 6, 0, b->transform.position.z));
    // They must sidestep (lateral z bulge) and never overlap (min separation
    // stays above roughly one agent radius); both arrive.
    bool pass = maxLat > 0.3f && minSep > 0.35f && aGoal < 1.0f && bGoal < 1.0f;
    std::printf("[NavSmoke] crowd: minSep=%.2f, maxLateral=%.2f, goals a=%.2f b=%.2f in %d -> %s\n",
                minSep, maxLat, aGoal, bGoal, frames, pass ? "PASS" : "FAIL");
    return pass;
}

// A guard facing +X sees a "Player" in front (within cone + range) but not one
// placed behind it — the sight-cone test, LOS off to isolate FOV geometry.
static bool testPerception() {
    World w;
    Entity* guard = w.spawn("Guard");
    guard->transform.position = Vec3(0, 0, 0);
    guard->transform.rotation = quatLookAt(Vec3(1, 0, 0)); // face +X
    auto* per = guard->addComponent<PerceptionComponent>();
    per->targetTag = "Player";
    per->sightRange = 12.0f;
    per->sightFovDeg = 90.0f;
    per->requireLineOfSight = false;

    Entity* front = w.spawn("PlayerFront");
    front->transform.position = Vec3(5, 0, 0.5f); // ahead, inside the cone
    Input in;
    const float dt = 1.0f / 60.0f;
    w.update(dt, 0.0f, in, true);
    bool seesFront = per->target() == front;

    // Move the player behind the guard: outside the cone -> not seen.
    front->transform.position = Vec3(-5, 0, 0);
    w.update(dt, dt, in, true);
    bool seesBehind = per->target() != nullptr;

    // Beyond range -> not seen.
    front->transform.position = Vec3(30, 0, 0);
    w.update(dt, 2 * dt, in, true);
    bool seesFar = per->target() != nullptr;

    bool pass = seesFront && !seesBehind && !seesFar;
    std::printf("[NavSmoke] perception: front=%d behind=%d far=%d -> %s\n", seesFront,
                seesBehind, seesFar, pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    bool ok = true;
    ok &= testPathAroundWall();
    ok &= testUnreachable();
    ok &= testAgentWalks();
    ok &= testDynamicObstacle();
    ok &= testCrowdAvoidance();
    ok &= testPerception();
    std::printf("[NavSmoke] %s\n", ok ? "ALL PASS" : "FAILURES");
    return ok ? 0 : 1;
}
