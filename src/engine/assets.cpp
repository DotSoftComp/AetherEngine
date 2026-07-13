#include "assets.h"
#include "../core/log.h"
#include "../render/image.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <fstream>

namespace ae {

unsigned AssetLibrary::uiImage(const std::string& path) {
    if (path.empty()) return 0;
    auto it = uiImages_.find(path);
    if (it != uiImages_.end()) return it->second.id();

    Texture2D& tex = uiImages_[path]; // insert (id() == 0 until loaded)
    std::ifstream f(resolvePath(path), std::ios::binary | std::ios::ate);
    if (!f) {
        AE_WARN("[UI] image not found: %s", path.c_str());
        return 0;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes(size);
    f.read((char*)bytes.data(), (std::streamsize)size);
    ImageData img;
    if (!decodeImage(bytes.data(), bytes.size(), img)) {
        AE_WARN("[UI] image decode failed: %s", path.c_str());
        return 0;
    }
    tex.create(img.width, img.height, img.rgba.data(), /*srgb=*/true);
    return tex.id();
}

static std::string normalizeSlashes(std::string s) {
    for (char& c : s)
        if (c == '\\') c = '/';
    return s;
}

void AssetLibrary::init(const std::string& projectRoot) {
    root_ = projectRoot;
    setTextureCacheDir(projectRoot.empty() ? std::string()
                                           : projectRoot + "/Intermediate/TextureCache");
    meshes_["sphere"].upload(makeSphere(0.5f, 48, 32));
    meshes_["cube"].upload(makeCube(0.5f));
    meshes_["plane"].upload(makePlane(40.0f, 26.0f));
    meshes_["torus"].upload(makeTorus(1.0f, 0.34f, 64, 32));
    makeTileTextures(texSets_["tile"], 512);
    makeRustedMetalTextures(texSets_["rust"], 512);
}

void AssetLibrary::destroy() {
    for (auto& kv : meshes_) kv.second.destroy();
    for (auto& kv : texSets_) {
        kv.second.albedo.destroy();
        kv.second.normal.destroy();
        kv.second.orm.destroy();
    }
    for (auto& kv : models_) kv.second->destroy();
    for (auto& kv : matGraphs_) {
        if (!kv.second) continue;
        kv.second->shader.destroy();
        for (auto& t : kv.second->textures) t.destroy();
    }
    meshes_.clear();
    texSets_.clear();
    models_.clear();
    matGraphs_.clear();
    dataTables_.clear();
}

const MaterialGraphAsset* AssetLibrary::materialGraph(const std::string& path) {
    std::string key = normalizeSlashes(toProjectRelative(path));
    auto it = matGraphs_.find(key);
    if (it != matGraphs_.end()) return it->second && it->second->valid ? it->second.get() : nullptr;

    auto asset = std::make_unique<MaterialGraphAsset>();
    bool ok = asset->compile(resolvePath(path));
    MaterialGraphAsset* raw = asset.get();
    matGraphs_[key] = std::move(asset); // negative results cached too (warn once)
    return ok ? raw : nullptr;
}

const DataTable* AssetLibrary::dataTable(const std::string& path) {
    std::string key = normalizeSlashes(toProjectRelative(path));
    auto it = dataTables_.find(key);
    if (it != dataTables_.end()) return it->second.get();

    auto table = std::make_unique<DataTable>();
    if (!table->load(resolvePath(path))) {
        AE_ERROR("[Data] failed to load table: %s", path.c_str());
        dataTables_[key] = nullptr; // cache the failure, don't retry every frame
        return nullptr;
    }
    AE_LOG("[Data] loaded table: %s (%d rows)", path.c_str(), table->rowCount());
    DataTable* raw = table.get();
    dataTables_[key] = std::move(table);
    return raw;
}

void AssetLibrary::reloadDataTable(const std::string& path) {
    std::string key = normalizeSlashes(toProjectRelative(path));
    auto it = dataTables_.find(key);
    if (it != dataTables_.end() && it->second) it->second->load(resolvePath(path));
    else {
        dataTables_.erase(key); // drop a cached failure so the next lookup retries
        dataTable(path);
    }
}

void AssetLibrary::reloadMaterialGraph(const std::string& path) {
    std::string key = normalizeSlashes(toProjectRelative(path));
    auto it = matGraphs_.find(key);
    if (it != matGraphs_.end() && it->second) it->second->compile(resolvePath(path));
    else materialGraph(path);
}

Mesh* AssetLibrary::mesh(const std::string& name) {
    auto it = meshes_.find(name);
    return it == meshes_.end() ? nullptr : &it->second;
}

const MaterialTextures* AssetLibrary::textureSet(const std::string& name) {
    auto it = texSets_.find(name);
    return it == texSets_.end() ? nullptr : &it->second;
}

std::string AssetLibrary::resolvePath(const std::string& maybeRelative) const {
    std::string p = normalizeSlashes(maybeRelative);
    if (p.size() > 1 && (p[1] == ':' || (p[0] == '/' && p[1] == '/'))) return p; // absolute
    return normalizeSlashes(root_) + "/" + p;
}

std::string AssetLibrary::toProjectRelative(const std::string& absolute) const {
    std::string a = normalizeSlashes(absolute), r = normalizeSlashes(root_);
    if (a.size() > r.size() + 1 && _strnicmp(a.c_str(), r.c_str(), r.size()) == 0)
        return a.substr(r.size() + 1);
    return a;
}

Model* AssetLibrary::model(const std::string& path) {
    std::string key = normalizeSlashes(toProjectRelative(path));
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    auto it = models_.find(key);
    if (it != models_.end()) return it->second ? it->second.get() : nullptr;

    auto m = std::make_unique<Model>();
    if (!m->load(resolvePath(path).c_str())) {
        AE_ERROR("[Assets] failed to load model: %s", path.c_str());
        models_[key] = nullptr; // cache the failure, don't retry every frame
        return nullptr;
    }
    AE_LOG("[Assets] loaded model: %s (%d clips)", path.c_str(), m->clipCount());
    Model* raw = m.get();
    models_[key] = std::move(m);
    return raw;
}

std::vector<std::string> AssetLibrary::meshNames() const {
    std::vector<std::string> out;
    for (const auto& kv : meshes_) out.push_back(kv.first);
    return out;
}

std::vector<std::string> AssetLibrary::textureSetNames() const {
    std::vector<std::string> out;
    for (const auto& kv : texSets_) out.push_back(kv.first);
    return out;
}

} // namespace ae
