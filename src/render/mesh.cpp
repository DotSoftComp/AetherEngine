#include "mesh.h"

namespace ae {

void computeTangents(MeshData& mesh) {
    std::vector<Vec3> tan(mesh.vertices.size(), Vec3(0));
    std::vector<Vec3> bitan(mesh.vertices.size(), Vec3(0));

    for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
        uint32_t i0 = mesh.indices[i], i1 = mesh.indices[i + 1], i2 = mesh.indices[i + 2];
        const Vertex& v0 = mesh.vertices[i0];
        const Vertex& v1 = mesh.vertices[i1];
        const Vertex& v2 = mesh.vertices[i2];

        Vec3 e1 = v1.position - v0.position;
        Vec3 e2 = v2.position - v0.position;
        float du1 = v1.uv.x - v0.uv.x, dv1 = v1.uv.y - v0.uv.y;
        float du2 = v2.uv.x - v0.uv.x, dv2 = v2.uv.y - v0.uv.y;

        float det = du1 * dv2 - du2 * dv1;
        if (std::fabs(det) < 1e-12f) continue;
        float r = 1.0f / det;
        Vec3 t = (e1 * dv2 - e2 * dv1) * r;
        Vec3 b = (e2 * du1 - e1 * du2) * r;

        tan[i0] += t; tan[i1] += t; tan[i2] += t;
        bitan[i0] += b; bitan[i1] += b; bitan[i2] += b;
    }

    for (size_t i = 0; i < mesh.vertices.size(); ++i) {
        Vec3 n = mesh.vertices[i].normal;
        Vec3 t = tan[i];
        t = normalize(t - n * dot(n, t)); // Gram-Schmidt
        float handedness = dot(cross(n, t), bitan[i]) < 0.0f ? -1.0f : 1.0f;
        mesh.vertices[i].tangent = Vec4(t, handedness);
    }
}

MeshData makeSphere(float radius, int segments, int rings) {
    MeshData m;
    for (int y = 0; y <= rings; ++y) {
        float v = (float)y / rings;
        float theta = v * PI;
        for (int x = 0; x <= segments; ++x) {
            float u = (float)x / segments;
            float phi = u * 2.0f * PI;
            Vec3 n(std::sin(theta) * std::cos(phi),
                   std::cos(theta),
                   std::sin(theta) * std::sin(phi));
            Vertex vert;
            vert.position = n * radius;
            vert.normal = n;
            vert.uv = Vec2(u * 2.0f, v);
            m.vertices.push_back(vert);
        }
    }
    for (int y = 0; y < rings; ++y) {
        for (int x = 0; x < segments; ++x) {
            uint32_t a = y * (segments + 1) + x;
            uint32_t b = a + segments + 1;
            // CCW as seen from outside (+phi runs "left" on screen from outside).
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    computeTangents(m);
    return m;
}

MeshData makeCube(float h) {
    MeshData m;
    const Vec3 faceNormals[6] = {
        {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    const Vec3 faceTangents[6] = {
        {0, 0, -1}, {0, 0, 1}, {1, 0, 0}, {1, 0, 0}, {1, 0, 0}, {-1, 0, 0}};
    for (int f = 0; f < 6; ++f) {
        Vec3 n = faceNormals[f];
        Vec3 t = faceTangents[f];
        Vec3 b = cross(n, t);
        uint32_t base = (uint32_t)m.vertices.size();
        for (int j = 0; j < 4; ++j) {
            float su = (j == 1 || j == 2) ? 1.0f : -1.0f;
            float sv = (j >= 2) ? 1.0f : -1.0f;
            Vertex v;
            v.position = (n + t * su + b * sv) * h;
            v.normal = n;
            v.tangent = Vec4(t, 1.0f);
            v.uv = Vec2(su * 0.5f + 0.5f, sv * 0.5f + 0.5f);
            m.vertices.push_back(v);
        }
        m.indices.insert(m.indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
    }
    computeTangents(m);
    return m;
}

MeshData makePlane(float h, float uvTiling) {
    MeshData m;
    const int grid = 8; // subdivided so per-vertex fog/attribute interpolation stays sane
    for (int y = 0; y <= grid; ++y) {
        for (int x = 0; x <= grid; ++x) {
            float u = (float)x / grid, v = (float)y / grid;
            Vertex vert;
            vert.position = Vec3((u * 2 - 1) * h, 0.0f, (v * 2 - 1) * h);
            vert.normal = Vec3(0, 1, 0);
            vert.tangent = Vec4(Vec3(1, 0, 0), 1.0f);
            vert.uv = Vec2(u * uvTiling, v * uvTiling);
            m.vertices.push_back(vert);
        }
    }
    for (int y = 0; y < grid; ++y) {
        for (int x = 0; x < grid; ++x) {
            uint32_t a = y * (grid + 1) + x;
            uint32_t b = a + grid + 1;
            m.indices.insert(m.indices.end(), {a, b, a + 1, a + 1, b, b + 1});
        }
    }
    return m;
}

MeshData makeTorus(float R, float r, int majorSegs, int minorSegs) {
    MeshData m;
    for (int i = 0; i <= majorSegs; ++i) {
        float u = (float)i / majorSegs;
        float phi = u * 2.0f * PI;
        Vec3 center(std::cos(phi) * R, 0.0f, std::sin(phi) * R);
        for (int j = 0; j <= minorSegs; ++j) {
            float v = (float)j / minorSegs;
            float theta = v * 2.0f * PI;
            Vec3 dir(std::cos(phi) * std::cos(theta), std::sin(theta), std::sin(phi) * std::cos(theta));
            Vertex vert;
            vert.position = center + dir * r;
            vert.normal = dir;
            vert.uv = Vec2(u * 8.0f, v * 2.0f);
            m.vertices.push_back(vert);
        }
    }
    for (int i = 0; i < majorSegs; ++i) {
        for (int j = 0; j < minorSegs; ++j) {
            uint32_t a = i * (minorSegs + 1) + j;
            uint32_t b = a + minorSegs + 1;
            m.indices.insert(m.indices.end(), {a, a + 1, b, a + 1, b + 1, b});
        }
    }
    computeTangents(m);
    return m;
}

MeshData makeCylinder(float radius, float halfHeight, int segments) {
    MeshData m;
    // Side: a ring pair, seam duplicated so U runs 0..1 without wrapping.
    for (int i = 0; i <= segments; ++i) {
        float u = (float)i / segments;
        float a = u * 2.0f * PI;
        Vec3 n(std::cos(a), 0.0f, std::sin(a));
        for (int j = 0; j < 2; ++j) {
            Vertex v;
            v.position = Vec3(n.x * radius, j ? halfHeight : -halfHeight, n.z * radius);
            v.normal = n;
            v.uv = Vec2(u, j ? 1.0f : 0.0f);
            m.vertices.push_back(v);
        }
    }
    for (int i = 0; i < segments; ++i) {
        uint32_t a = (uint32_t)i * 2;
        m.indices.insert(m.indices.end(), {a, a + 1, a + 3, a, a + 3, a + 2});
    }
    // Caps: one center vertex per end plus a fan (flat normals, so the rim
    // stays a hard edge instead of smearing the side's normals over the top).
    for (int cap = 0; cap < 2; ++cap) {
        float y = cap ? halfHeight : -halfHeight;
        Vec3 n(0.0f, cap ? 1.0f : -1.0f, 0.0f);
        uint32_t center = (uint32_t)m.vertices.size();
        Vertex c;
        c.position = Vec3(0, y, 0);
        c.normal = n;
        c.uv = Vec2(0.5f, 0.5f);
        m.vertices.push_back(c);
        for (int i = 0; i <= segments; ++i) {
            float a = (float)i / segments * 2.0f * PI;
            Vertex v;
            v.position = Vec3(std::cos(a) * radius, y, std::sin(a) * radius);
            v.normal = n;
            v.uv = Vec2(std::cos(a) * 0.5f + 0.5f, std::sin(a) * 0.5f + 0.5f);
            m.vertices.push_back(v);
        }
        for (int i = 0; i < segments; ++i) {
            uint32_t a = center + 1 + i, b = center + 2 + i;
            if (cap) m.indices.insert(m.indices.end(), {center, a, b});
            else m.indices.insert(m.indices.end(), {center, b, a}); // flip winding
        }
    }
    computeTangents(m);
    return m;
}

MeshData makeQuad(float halfExtent) {
    MeshData m;
    // Faces +Z, centered on the origin: the billboard/decal/poster primitive.
    const Vec2 corners[4] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
    for (const Vec2& c : corners) {
        Vertex v;
        v.position = Vec3(c.x * halfExtent, c.y * halfExtent, 0.0f);
        v.normal = Vec3(0, 0, 1);
        v.tangent = Vec4(Vec3(1, 0, 0), 1.0f);
        v.uv = Vec2(c.x * 0.5f + 0.5f, 0.5f - c.y * 0.5f); // v down = image order
        m.vertices.push_back(v);
    }
    m.indices = {0, 1, 2, 0, 2, 3};
    computeTangents(m);
    return m;
}

void Mesh::upload(const MeshData& data, bool dynamic, bool keepNavGeo) {
    indexCount_ = (unsigned)data.indices.size();

    if (keepNavGeo) {
        navPositions_.resize(data.vertices.size());
        for (size_t i = 0; i < data.vertices.size(); ++i)
            navPositions_[i] = data.vertices[i].position;
        navIndices_ = data.indices;
    }

    if (!data.vertices.empty()) {
        boundsMin_ = boundsMax_ = data.vertices[0].position;
        for (const Vertex& v : data.vertices) {
            const Vec3& p = v.position;
            if (p.x < boundsMin_.x) boundsMin_.x = p.x;
            if (p.y < boundsMin_.y) boundsMin_.y = p.y;
            if (p.z < boundsMin_.z) boundsMin_.z = p.z;
            if (p.x > boundsMax_.x) boundsMax_.x = p.x;
            if (p.y > boundsMax_.y) boundsMax_.y = p.y;
            if (p.z > boundsMax_.z) boundsMax_.z = p.z;
        }
    }

    static const rhi::VertexAttr attrs[] = {
        {0, 3, offsetof(Vertex, position)},
        {1, 3, offsetof(Vertex, normal)},
        {2, 4, offsetof(Vertex, tangent)},
        {3, 2, offsetof(Vertex, uv)},
        {4, 4, offsetof(Vertex, joints)},
        {5, 4, offsetof(Vertex, weights)},
    };
    rhi::GeometryDesc desc;
    desc.vertexData = data.vertices.data();
    desc.vertexBytes = data.vertices.size() * sizeof(Vertex);
    desc.vertexStride = sizeof(Vertex);
    desc.indexData = data.indices.data();
    desc.indexCount = data.indices.size();
    desc.attrs = attrs;
    desc.attrCount = 6;
    desc.dynamic = dynamic;
    geom_ = rhi::createGeometry(desc);
}

void Mesh::updateVertices(const std::vector<Vertex>& vertices) {
    rhi::updateGeometryVertices(geom_, vertices.data(), vertices.size() * sizeof(Vertex));
}

void Mesh::destroy() { rhi::destroyGeometry(geom_); }

void Mesh::draw() const { rhi::draw(geom_, indexCount_); }

void Mesh::drawInstanced(int instances) const { rhi::draw(geom_, indexCount_, instances); }

} // namespace ae
