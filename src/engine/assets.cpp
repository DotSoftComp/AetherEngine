#include "assets.h"
#include "../core/log.h"
#include "../core/json.h"
#include "../render/image.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace ae {

namespace {
namespace fs = std::filesystem;

// Last-write time as a plain integer (0 = missing). Sidecars count too: an
// import-settings edit must re-import just like a pixel edit.
int64_t writeStamp(const std::string& path) {
    std::error_code ec;
    auto t = fs::last_write_time(fs::u8path(path), ec);
    if (ec) return 0;
    return (int64_t)t.time_since_epoch().count();
}
int64_t sourceStamp(const std::string& path) {
    // The sidecar's stamp folds in, so editing <img>.import.json re-imports.
    int64_t a = writeStamp(path);
    int64_t b = writeStamp(path + ".import.json");
    return a * 31 + b;
}
} // namespace

void AssetLibrary::trackSource(const std::string& absSource, DepKind kind,
                               const std::string& owner) {
    auto& list = deps_[absSource];
    for (const SourceDep& d : list)
        if (d.kind == kind && d.owner == owner) return; // already tracked
    list.push_back({kind, owner, sourceStamp(absSource)});
}

unsigned AssetLibrary::uiImage(const std::string& path) {
    if (path.empty()) return 0;
    auto it = uiImages_.find(path);
    if (it != uiImages_.end()) return it->second.id();

    Texture2D& tex = uiImages_[path]; // insert (id() == 0 until loaded)
    std::string abs = resolvePath(path);
    trackSource(abs, DepKind::UIImage, path);
    std::ifstream f(abs, std::ios::binary | std::ios::ate);
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
    // UI sprites are sRGB and uncompressed by default; a sidecar can still cap
    // the size or turn compression on.
    TextureImportSettings ts;
    ts.srgb = true;
    ts.compress = false;
    ts = loadImportSettings(abs, ts);
    tex.createImported(img.width, img.height, img.rgba.data(), ts,
                       contentHash64(bytes.data(), bytes.size()));
    return tex.id();
}

int AssetLibrary::pollSourceChanges() {
    // Throttle: stat'ing every source file each frame would be wasteful.
    double now = (double)GetTickCount64() * 0.001;
    if (now - lastPoll_ < 0.5) return 0;
    lastPoll_ = now;

    // Collect first: reloading mutates deps_ (re-tracking sources).
    struct Hit { DepKind kind; std::string owner; };
    std::vector<Hit> hits;
    for (auto& [source, list] : deps_) {
        int64_t stamp = sourceStamp(source);
        for (SourceDep& d : list) {
            if (stamp == d.stamp) continue;
            d.stamp = stamp;
            if (stamp == 0) continue; // deleted: keep the last good import
            bool dup = false;
            for (const Hit& h : hits)
                if (h.kind == d.kind && h.owner == d.owner) { dup = true; break; }
            if (!dup) hits.push_back({d.kind, d.owner});
        }
    }

    int reloaded = 0;
    for (const Hit& h : hits) {
        if (h.kind == DepKind::UIImage) {
            auto it = uiImages_.find(h.owner);
            if (it == uiImages_.end()) continue;
            // Re-import in place: createImported reuses the rhi slot, so the id
            // every material/UI widget cached stays valid.
            std::string abs = resolvePath(h.owner);
            std::ifstream f(abs, std::ios::binary | std::ios::ate);
            if (!f) continue;
            size_t size = (size_t)f.tellg();
            f.seekg(0);
            std::vector<uint8_t> bytes(size);
            f.read((char*)bytes.data(), (std::streamsize)size);
            ImageData img;
            if (!decodeImage(bytes.data(), bytes.size(), img)) continue;
            TextureImportSettings ts;
            ts.srgb = true;
            ts.compress = false;
            ts = loadImportSettings(abs, ts);
            it->second.createImported(img.width, img.height, img.rgba.data(), ts,
                                      contentHash64(bytes.data(), bytes.size()));
            AE_LOG("[Assets] re-imported image %s", h.owner.c_str());
            ++reloaded;
        } else if (h.kind == DepKind::MaterialGraph) {
            reloadMaterialGraph(h.owner); // recompiles + re-imports its textures
            AE_LOG("[Assets] re-imported material graph %s", h.owner.c_str());
            ++reloaded;
        }
    }
    return reloaded;
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
    // keepNavGeo: retain CPU triangles so navStatic MeshRenderers can bake nav.
    meshes_["sphere"].upload(makeSphere(0.5f, 48, 32), false, true);
    meshes_["cube"].upload(makeCube(0.5f), false, true);
    meshes_["plane"].upload(makePlane(40.0f, 26.0f), false, true);
    meshes_["torus"].upload(makeTorus(1.0f, 0.34f, 64, 32), false, true);
    meshes_["cylinder"].upload(makeCylinder(0.5f, 0.5f, 32), false, true);
    meshes_["quad"].upload(makeQuad(0.5f), false, true);
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
    for (const std::string& src : asset->sources)
        trackSource(src, DepKind::MaterialGraph, key); // graph + subgraphs + textures
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
    if (it != matGraphs_.end() && it->second) {
        it->second->compile(resolvePath(path));
        // The recompile may have picked up different subgraphs/textures.
        for (const std::string& src : it->second->sources)
            trackSource(src, DepKind::MaterialGraph, key);
    } else {
        materialGraph(path);
    }
}

Mesh* AssetLibrary::mesh(const std::string& name) {
    auto it = meshes_.find(name);
    return it == meshes_.end() ? nullptr : &it->second;
}

// Loads one channel of a .texset.json. Missing key or missing file leaves the
// texture unset, which the material treats as "use the default white/flat map"
// — so a set can ship albedo-only and still render.
bool AssetLibrary::loadTexSetChannel(Texture2D& out, const JsonValue& set, const char* key,
                                     bool srgb, bool normalMap) {
    const std::string* rel = set.string(key);
    if (!rel || rel->empty()) return false;
    std::string abs = resolvePath(*rel);
    std::ifstream f(abs, std::ios::binary | std::ios::ate);
    if (!f) {
        AE_WARN("[Assets] texture set: missing %s image %s", key, rel->c_str());
        return false;
    }
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes(size);
    f.read((char*)bytes.data(), (std::streamsize)size);
    ImageData img;
    if (!decodeImage(bytes.data(), bytes.size(), img)) {
        AE_WARN("[Assets] texture set: cannot decode %s", rel->c_str());
        return false;
    }
    TextureImportSettings ts;
    ts.srgb = srgb;          // color maps only; normal/ORM are data
    ts.normalMap = normalMap; // BC5 two-channel, Z rebuilt in the shader
    ts = loadImportSettings(abs, ts); // a sidecar still wins
    out.createImported(img.width, img.height, img.rgba.data(), ts,
                       contentHash64(bytes.data(), bytes.size()));
    return true;
}

const MaterialTextures* AssetLibrary::textureSet(const std::string& name) {
    std::string key = normalizeSlashes(name);
    auto it = texSets_.find(key);
    if (it != texSets_.end()) return &it->second;
    // Not a built-in: treat the name as a project-relative .texset.json — a
    // three-line asset naming the albedo/normal/orm images. This is what lets a
    // project ship its own surfaces without touching the engine.
    if (key.find('/') == std::string::npos && key.find(".json") == std::string::npos)
        return nullptr; // a plain unknown name, not a path — don't hit the disk

    std::string abs = resolvePath(key);
    std::ifstream f(abs, std::ios::binary | std::ios::ate);
    JsonValue set;
    bool ok = false;
    if (f) {
        size_t size = (size_t)f.tellg();
        f.seekg(0);
        std::string text(size, '\0');
        f.read(&text[0], (std::streamsize)size);
        ok = jsonParse(text.c_str(), text.size(), set);
    }
    if (!ok) {
        AE_ERROR("[Assets] texture set not found or malformed: %s", name.c_str());
        texSets_[key]; // cache the empty set so we warn once, not every frame
        return nullptr;
    }
    MaterialTextures& t = texSets_[key];
    loadTexSetChannel(t.albedo, set, "albedo", /*srgb=*/true, /*normalMap=*/false);
    loadTexSetChannel(t.normal, set, "normal", /*srgb=*/false, /*normalMap=*/true);
    loadTexSetChannel(t.orm, set, "orm", /*srgb=*/false, /*normalMap=*/false);
    AE_LOG("[Assets] loaded texture set: %s", key.c_str());
    return &t;
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
