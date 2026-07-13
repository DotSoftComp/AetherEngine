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

class Mesh {
public:
    // `dynamic` marks the vertex buffer for streaming updates (morph targets).
    void upload(const MeshData& data, bool dynamic = false);
    // Re-uploads vertex data in place (same vertex count/layout as upload).
    void updateVertices(const std::vector<Vertex>& vertices);
    void destroy();
    void draw() const;
    void drawInstanced(int instances) const;

    // Local-space AABB, computed at upload (for frustum culling and picking).
    Vec3 boundsMin() const { return boundsMin_; }
    Vec3 boundsMax() const { return boundsMax_; }

private:
    rhi::GeometryHandle geom_;
    unsigned indexCount_ = 0;
    Vec3 boundsMin_{0, 0, 0}, boundsMax_{0, 0, 0};
};

} // namespace ae
