// Aether Engine — named asset registry. Scene files and editor tooling refer
// to assets through one place: procedural meshes and texture sets by name,
// glTF models by project-relative path (lazy-loaded and cached). Everything
// registered here outlives the World, so components can hold raw pointers.
#pragma once
#include "data_table.h"
#include "../render/mesh.h"
#include "../render/texture.h"
#include "../render/gltf.h"
#include "../render/material_graph.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ae {

struct JsonValue;

class AssetLibrary {
public:
    // Builds the procedural meshes ("sphere","cube","plane","torus") and
    // texture sets ("tile","rust"). `projectRoot` anchors relative model paths.
    void init(const std::string& projectRoot);
    void destroy();

    const std::string& projectRoot() const { return root_; }

    Mesh* mesh(const std::string& name);
    // A built-in name ("tile", "rust") or a project-relative `.texset.json`
    // naming albedo/normal/orm images — the path form is how a project ships
    // its own surfaces. Loaded lazily and cached; nullptr if it can't be read.
    const MaterialTextures* textureSet(const std::string& name);
    // Accepts a project-relative ("assets/Fox.glb") or absolute path.
    // Returns nullptr if the file can't be loaded.
    Model* model(const std::string& path);

    // Compiled material graph (lazy, cached). The returned pointer is stable;
    // reloadMaterialGraph recompiles in place (editor hot reload). Returns
    // nullptr when the file is missing or has no Output node.
    const MaterialGraphAsset* materialGraph(const std::string& path);
    void reloadMaterialGraph(const std::string& path);

    // Data table (lazy, cached). Stable pointer; reloadDataTable re-reads the
    // file in place (editor hot reload). Returns nullptr when missing/malformed.
    const DataTable* dataTable(const std::string& path);
    void reloadDataTable(const std::string& path);

    std::vector<std::string> meshNames() const;
    std::vector<std::string> textureSetNames() const;

    // UI image (sprite) loaded from a project-relative PNG/JPG path, cached by
    // path; returns the rhi texture id (0 when missing). For UIDocument Image
    // widgets — sRGB, no mips.
    unsigned uiImage(const std::string& path);

    std::string resolvePath(const std::string& maybeRelative) const;
    std::string toProjectRelative(const std::string& absolute) const;

    // ---- dependency tracking / hot re-import ----
    // Every imported asset records the source files it was built from (the
    // image, and for material graphs the .json + each texture it samples).
    // pollSourceChanges() stats those files and re-imports whatever changed,
    // in place — re-imported textures keep their rhi id, so live materials and
    // UI documents pick the new pixels up with no rebinding. Returns the number
    // of assets reloaded. The editor calls this on a timer; safe to call every
    // frame (it throttles internally).
    int pollSourceChanges();

private:
    // What to re-run when a tracked source file changes on disk.
    enum class DepKind { UIImage, MaterialGraph };
    struct SourceDep {
        DepKind kind;
        std::string owner;   // asset key (uiImages_ / matGraphs_ map key)
        int64_t stamp = 0;   // last-seen write time (0 = missing)
    };
    // source file (absolute) -> the assets built from it.
    std::map<std::string, std::vector<SourceDep>> deps_;
    void trackSource(const std::string& absSource, DepKind kind, const std::string& owner);
    bool loadTexSetChannel(Texture2D& out, const JsonValue& set, const char* key, bool srgb,
                           bool normalMap);
    double lastPoll_ = 0.0;

    std::string root_;
    std::map<std::string, Mesh> meshes_;
    std::map<std::string, MaterialTextures> texSets_;
    std::map<std::string, std::unique_ptr<Model>> models_; // key: normalized relative path
    std::map<std::string, std::unique_ptr<MaterialGraphAsset>> matGraphs_;
    std::map<std::string, std::unique_ptr<DataTable>> dataTables_;
    std::map<std::string, Texture2D> uiImages_; // key: normalized relative path
};

} // namespace ae
