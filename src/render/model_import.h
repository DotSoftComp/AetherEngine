// Aether Engine — shared scaffolding for static-model importers (OBJ, FBX).
// Collects meshes/materials/draws and commits them into a Model's internals in
// one go (GPU upload last, so vector growth never moves a live GL object).
#pragma once
#include "gltf.h"
#include "image.h"
#include "../core/log.h"
#include <fstream>
#include <string>
#include <vector>

namespace ae {

struct ModelBuild {
    Model& m;
    std::vector<MeshData> meshData;
    std::vector<Material> materials;
    std::vector<std::pair<int, int>> drawList; // (meshData idx, material idx)
    std::vector<Mat4> nodeForDraw;             // world transform per draw

    explicit ModelBuild(Model& model) : m(model) {}

    int addMaterial(const Material& mat) {
        materials.push_back(mat);
        return (int)materials.size() - 1;
    }
    void addDraw(const MeshData& data, int material, const Mat4& world = Mat4::identity()) {
        if (data.vertices.empty() || data.indices.empty()) return;
        meshData.push_back(data);
        drawList.push_back({(int)meshData.size() - 1, material});
        nodeForDraw.push_back(world);
    }

    // Loads an image file into a Model-owned texture; 0 when missing/undecodable.
    unsigned loadTexture(const std::string& path, bool srgb) {
        if (path.empty()) return 0;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) {
            AE_WARN("[Import] missing texture: %s", path.c_str());
            return 0;
        }
        size_t size = (size_t)f.tellg();
        f.seekg(0);
        std::vector<uint8_t> bytes(size);
        f.read((char*)bytes.data(), (std::streamsize)size);
        ImageData img;
        if (!decodeImage(bytes.data(), bytes.size(), img)) return 0;
        m.textures_.emplace_back();
        m.textures_.back().createCompressed(img.width, img.height, img.rgba.data(), srgb,
                                            contentHash64(bytes.data(), bytes.size()));
        return m.textures_.back().id();
    }

    // Uploads everything into the Model: one node per draw (baked transform).
    bool commit() {
        if (meshData.empty()) return false;
        m.materials_ = materials;
        m.meshes_.resize(meshData.size());
        bool first = true;
        for (size_t i = 0; i < meshData.size(); ++i) {
            computeTangents(meshData[i]);
            m.meshes_[i].upload(meshData[i]);
            Vec3 mn = m.meshes_[i].boundsMin(), mx = m.meshes_[i].boundsMax();
            if (first) { m.boundsMin_ = mn; m.boundsMax_ = mx; first = false; }
            m.boundsMin_ = Vec3(std::min(m.boundsMin_.x, mn.x), std::min(m.boundsMin_.y, mn.y),
                                std::min(m.boundsMin_.z, mn.z));
            m.boundsMax_ = Vec3(std::max(m.boundsMax_.x, mx.x), std::max(m.boundsMax_.y, mx.y),
                                std::max(m.boundsMax_.z, mx.z));
        }
        for (size_t d = 0; d < drawList.size(); ++d) {
            m.nodes_.emplace_back();
            m.nodes_.back().hasMatrix = true;
            m.nodes_.back().matrix = nodeForDraw[d];
            m.roots_.push_back((int)m.nodes_.size() - 1);
            Model::Draw draw;
            draw.node = (int)m.nodes_.size() - 1;
            draw.mesh = drawList[d].first;
            draw.material = drawList[d].second;
            draw.skin = -1;
            m.draws_.push_back(draw);
        }
        m.computeWorlds();
        return true;
    }
};

} // namespace ae
