// Aether Engine — navigation mesh (Recast/Detour, the library Unreal uses).
//
// bake() voxelizes world triangles into walkable polygons sized for an agent
// (radius/height/step/slope); findPath() then answers point-to-point queries
// with a string-pulled waypoint list. The World owns one NavMesh (world.nav):
// the editor bakes it visibly (Tools > Bake Navmesh, View > Navmesh overlay),
// and Play/runtime auto-bakes on the first agent query if nobody baked yet.
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
};

class NavMesh {
public:
    NavMesh();
    ~NavMesh();
    NavMesh(const NavMesh&) = delete;
    NavMesh& operator=(const NavMesh&) = delete;

    // Gathers static geometry from the world's colliders (entities without a
    // RigidBody, or with a Static one; triggers skipped) and builds the mesh.
    bool bake(const World& world, const NavBuildSettings& s = {});
    void clear();
    bool valid() const;

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
};

// Collects navmesh input triangles from the world's static colliders.
// Exposed for tests; bake() uses it internally.
void gatherNavTriangles(const World& world, std::vector<float>& verts,
                        std::vector<int>& tris);

} // namespace ae
