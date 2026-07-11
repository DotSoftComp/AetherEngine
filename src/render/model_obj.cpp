// Aether Engine — Wavefront OBJ/MTL importer (hand-written, like the glTF
// loader). Static geometry: positions/UVs/normals, n-gon fan triangulation,
// negative indices, per-material submeshes; MTL diffuse color/texture,
// specular exponent mapped to roughness, dissolve to alpha.
#include "model_import.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <sstream>

namespace ae {

namespace {

struct ObjIndex {
    int v = 0, vt = 0, vn = 0;
    bool operator<(const ObjIndex& o) const {
        if (v != o.v) return v < o.v;
        if (vt != o.vt) return vt < o.vt;
        return vn < o.vn;
    }
};

// "3/7/2", "3//2", "3/7", "3", with negative (relative) indices.
ObjIndex parseIndex(const char* s, int nv, int nvt, int nvn) {
    ObjIndex ix;
    int vals[3] = {0, 0, 0};
    int part = 0;
    const char* p = s;
    while (*p && part < 3) {
        vals[part] = std::atoi(p);
        while (*p && *p != '/') ++p;
        if (*p == '/') { ++p; ++part; continue; }
        break;
    }
    auto fix = [](int i, int n) { return i > 0 ? i : (i < 0 ? n + i + 1 : 0); };
    ix.v = fix(vals[0], nv);
    ix.vt = fix(vals[1], nvt);
    ix.vn = fix(vals[2], nvn);
    return ix;
}

struct Mtl {
    Material mat;
    std::string mapKd;
};

void loadMtl(const std::string& path, std::map<std::string, Mtl>& out) {
    std::ifstream f(path);
    if (!f) {
        AE_WARN("[OBJ] missing material library: %s", path.c_str());
        return;
    }
    Mtl* cur = nullptr;
    std::string line;
    while (std::getline(f, line)) {
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "newmtl") {
            std::string name;
            ss >> name;
            cur = &out[name];
        } else if (cur && tag == "Kd") {
            float r, g, b;
            if (ss >> r >> g >> b) cur->mat.baseColor = Vec4(r, g, b, cur->mat.baseColor.w);
        } else if (cur && tag == "Ke") {
            float r, g, b;
            if (ss >> r >> g >> b) cur->mat.emissive = Vec3(r, g, b);
        } else if (cur && tag == "Ns") {
            float ns;
            // Blinn-Phong exponent -> GGX roughness (common approximation).
            if (ss >> ns) cur->mat.roughness = clampf(std::sqrt(2.0f / (ns + 2.0f)), 0.04f, 1.0f);
        } else if (cur && tag == "d") {
            float d;
            if (ss >> d) cur->mat.baseColor.w = d;
        } else if (cur && tag == "map_Kd") {
            std::string rest;
            std::getline(ss, rest);
            size_t a = rest.find_first_not_of(" \t");
            if (a != std::string::npos) cur->mapKd = rest.substr(a);
        }
    }
}

} // namespace

bool Model::loadObj(const char* path) {
    std::ifstream f(path);
    if (!f) {
        AE_ERROR("[OBJ] cannot open %s", path);
        return false;
    }
    std::string p(path);
    size_t slash = p.find_last_of("\\/");
    std::string baseDir = slash == std::string::npos ? "" : p.substr(0, slash + 1);

    std::vector<Vec3> vs, vns;
    std::vector<Vec2> vts;
    std::map<std::string, Mtl> mtls;

    // One MeshData per material name ("" = default), deduplicating vertices.
    struct Group {
        MeshData data;
        std::map<ObjIndex, uint32_t> lookup;
    };
    std::map<std::string, Group> groups;
    std::string curMtl;

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "v") {
            Vec3 v;
            ss >> v.x >> v.y >> v.z;
            vs.push_back(v);
        } else if (tag == "vt") {
            Vec2 t;
            ss >> t.x >> t.y;
            t.y = 1.0f - t.y; // OBJ UV origin is bottom-left
            vts.push_back(t);
        } else if (tag == "vn") {
            Vec3 n;
            ss >> n.x >> n.y >> n.z;
            vns.push_back(n);
        } else if (tag == "mtllib") {
            std::string lib;
            std::getline(ss, lib);
            size_t a = lib.find_first_not_of(" \t");
            if (a != std::string::npos) loadMtl(baseDir + lib.substr(a), mtls);
        } else if (tag == "usemtl") {
            ss >> curMtl;
        } else if (tag == "f") {
            Group& g = groups[curMtl];
            std::vector<uint32_t> face;
            std::string tok;
            while (ss >> tok) {
                ObjIndex ix = parseIndex(tok.c_str(), (int)vs.size(), (int)vts.size(),
                                         (int)vns.size());
                if (ix.v <= 0 || ix.v > (int)vs.size()) continue;
                auto it = g.lookup.find(ix);
                uint32_t vi;
                if (it != g.lookup.end()) {
                    vi = it->second;
                } else {
                    Vertex vert;
                    vert.position = vs[ix.v - 1];
                    if (ix.vt > 0 && ix.vt <= (int)vts.size()) vert.uv = vts[ix.vt - 1];
                    if (ix.vn > 0 && ix.vn <= (int)vns.size()) vert.normal = vns[ix.vn - 1];
                    vi = (uint32_t)g.data.vertices.size();
                    g.data.vertices.push_back(vert);
                    g.lookup[ix] = vi;
                }
                face.push_back(vi);
            }
            for (size_t i = 2; i < face.size(); ++i) { // fan-triangulate n-gons
                g.data.indices.push_back(face[0]);
                g.data.indices.push_back(face[i - 1]);
                g.data.indices.push_back(face[i]);
            }
        }
    }

    // Faces without normals: generate flat-averaged ones.
    for (auto& kv : groups) {
        MeshData& d = kv.second.data;
        bool hasNormals = false;
        for (const auto& v : d.vertices)
            if (dot(v.normal, v.normal) > 0.001f) { hasNormals = true; break; }
        if (hasNormals) continue;
        for (size_t i = 0; i + 2 < d.indices.size(); i += 3) {
            Vertex& a = d.vertices[d.indices[i]];
            Vertex& b = d.vertices[d.indices[i + 1]];
            Vertex& c = d.vertices[d.indices[i + 2]];
            Vec3 n = cross(b.position - a.position, c.position - a.position);
            a.normal += n;
            b.normal += n;
            c.normal += n;
        }
        for (auto& v : d.vertices) v.normal = normalize(v.normal);
    }

    ModelBuild build(*this);
    for (auto& kv : groups) {
        Material mat;
        auto mit = mtls.find(kv.first);
        if (mit != mtls.end()) {
            mat = mit->second.mat;
            if (!mit->second.mapKd.empty())
                mat.albedoTex = build.loadTexture(baseDir + mit->second.mapKd, /*srgb=*/true);
            if (mat.baseColor.w < 0.999f) mat.blend = true;
        }
        int mi = build.addMaterial(mat);
        build.addDraw(kv.second.data, mi);
    }
    if (!build.commit()) {
        AE_ERROR("[OBJ] no geometry in %s", path);
        return false;
    }
    AE_LOG("[OBJ] loaded %s (%d submeshes, %d materials)", path, (int)draws_.size(),
           (int)materials_.size());
    return true;
}

} // namespace ae
