// Aether Engine — navigation mesh (Recast/Detour, the library Unreal uses).
//
// bake() voxelizes world triangles into a TILED navmesh built through a
// dtTileCache, sized for an agent (radius/height/step/slope). Because the mesh
// is tiled and cached, DYNAMIC OBSTACLES can be carved into it at runtime
// (addObstacle) and only the affected tiles rebuild — no full rebake. A
// dtCrowd rides the same mesh so agents locally avoid each other and the
// obstacles (DetourCrowd RVO steering).
//
// findPath() answers point-to-point queries with a string-pulled waypoint
// list. The World owns one NavMesh (world.nav): the editor bakes it visibly
// (Tools > Bake Navmesh, View > Navmesh overlay), Play/runtime auto-bakes on
// the first agent query if nobody baked yet, and World::update advances
// obstacle rebuilds + crowd steering each frame.
#pragma once
#include "../core/math3d.h"
#include <memory>
#include <vector>

namespace ae {

class World;

struct NavBuildSettings {
    float cellSize = 0.25f;    // XZ voxel size (m) — smaller = tighter fit, slower
    float cellHeight = 0.2f;   // Y voxel size (m)
    float agentRadius = 0.4f;  // walkable polys shrink by this much from walls
    float agentHeight = 1.8f;  // min clearance under overhangs
    float agentMaxClimb = 0.4f;// max step height (stairs, curbs)
    float agentMaxSlope = 45.0f; // degrees
    int tileSize = 48;         // voxels per tile edge (tiled cache granularity)
};

// Runtime obstacle handle (dtObstacleRef); 0 = invalid / failed.
using NavObstacle = unsigned int;

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Gathers static geometry — the world's colliders (entities without a
    // RigidBody, or with a Static one; triggers skipped) plus any MeshRenderer
    // or Model flagged navStatic (their real triangles) — and builds the mesh.
    bool bake(const World& world, const NavBuildSettings& s = {});
    void clear();
    bool valid() const;

    // Advances dynamic-obstacle tile rebuilds and crowd steering. Call once per
    // frame (World::update does). dt in seconds.
    void update(float dt, const World& world);

    // ---- dynamic obstacles (rebake-free) ----
    // Carves a vertical cylinder out of the walkable area; the touched tiles
    // rebuild over the next few update() calls. 0 on failure (no mesh / full).
    NavObstacle addObstacle(const Vec3& pos, float radius, float height);
    void removeObstacle(NavObstacle id);

    // ---- crowd (local avoidance) ----
    // Registers an agent at pos; returns its crowd index, or -1 if unavailable.
    // The caller re-registers when generation() changes (a rebake resets the
    // crowd). radius/height size the agent; maxSpeed caps its steering.
    int addAgent(const Vec3& pos, float radius, float height, float maxSpeed);
    void removeAgent(int idx);
    void setAgentTarget(int idx, const Vec3& target);
    void resetAgentTarget(int idx);
    void setAgentParams(int idx, float maxSpeed, float radius);
    Vec3 agentPosition(int idx) const;
    Vec3 agentVelocity(int idx) const;
    bool agentValid(int idx) const;
    // Bumps on every successful bake; agents compare it to know when to
    // re-register after the crowd was rebuilt.
    unsigned generation() const { return generation_; }

    // String-pulled path start -> end (both snapped to the mesh within
    // `snapRadius`). Returns false when either point is off-mesh or no path
    // exists; `out` gets the waypoints including the (snapped) endpoints.
    bool findPath(const Vec3& start, const Vec3& end, std::vector<Vec3>& out,
                  float snapRadius = 2.0f) const;

    // Nearest point on the mesh (agent spawn snapping). Returns `p` unchanged
    // when nothing is within `snapRadius`.
    Vec3 nearestPoint(const Vec3& p, float snapRadius = 2.0f) const;

    // Debug visualization: appends world-space line segments (pairs of points)
    // outlining the walkable polygons.
    void debugLines(std::vector<Vec3>& out) const;

    int polyCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    unsigned generation_ = 0;
};

// Collects navmesh input triangles from the world's static colliders and any
// navStatic-flagged render meshes. Exposed for tests; bake() uses it.
void gatherNavTriangles(const World& world, std::vector<float>& verts,
                        std::vector<int>& tris);

} // namespace ae
