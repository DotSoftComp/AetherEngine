// Aether Engine — GPU mesh + procedural geometry (positions, normals, tangents, UVs).
#pragma once
#include "../rhi/rhi.h"
#include "../core/math3d.h"
#include <vector>
#include <cstdint>

namespace ae {

struct Vertex {
    Vec3 position;
    Vec3 normal;
    Vec4 tangent;   // xyz = tangent, w = bitangent handedness (+1/-1)
    Vec2 uv;
    // Skinning palette indices, stored as float for maximally portable
    // attribute fetch (exact for indices < 2^24; cast back to uint in shader).
    float joints[4] = {0, 0, 0, 0};
    float weights[4] = {0, 0, 0, 0};   // all zero = rigid vertex
};

struct MeshData {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
};

// Accumulates per-triangle tangents (MikkTSpace-style averaging, Gram-Schmidt orthogonalized).
void computeTangents(MeshData& mesh);

MeshData makeSphere(float radius, int segments, int rings);
MeshData makeCube(float halfExtent);
MeshData makePlane(float halfExtent, float uvTiling);
MeshData makeTorus(float majorRadius, float minorRadius, int majorSegments, int minorSegments);
// Capped cylinder around the Y axis (columns, barrels, pipes, tech pillars).
MeshData makeCylinder(float radius, float halfHeight, int segments);
// Single unit quad facing +Z (billboards, decals, posters, screens).
MeshData makeQuad(float halfExtent);

class Mesh {
public:
    // `dynamic` marks the vertex buffer for streaming updates (morph targets).
    // `keepNavGeo` retains a light CPU copy (positions + indices) so the mesh
    // can feed navmesh baking — set for asset/model meshes, not UI/particles.
    void upload(const MeshData& data, bool dynamic = false, bool keepNavGeo = false);
    // Re-uploads vertex data in place (same vertex count/layout as upload).
    void updateVertices(const std::vector<Vertex>& vertices);
    void destroy();
    void draw() const;
    void drawInstanced(int instances) const;

    // Local-space AABB, computed at upload (for frustum culling and picking).
    Vec3 boundsMin() const { return boundsMin_; }
    Vec3 boundsMax() const { return boundsMax_; }

    // CPU triangle geometry retained when uploaded with keepNavGeo (empty
    // otherwise). Positions are model-local; navIndices index into them.
    const std::vector<Vec3>& navPositions() const { return navPositions_; }
    const std::vector<uint32_t>& navIndices() const { return navIndices_; }

private:
    rhi::GeometryHandle geom_;
    unsigned indexCount_ = 0;
    Vec3 boundsMin_{0, 0, 0}, boundsMax_{0, 0, 0};
    std::vector<Vec3> navPositions_;   // model-local positions (keepNavGeo)
    std::vector<uint32_t> navIndices_; // triangle indices (keepNavGeo)
};

} // namespace ae
