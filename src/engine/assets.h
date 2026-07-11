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

class AssetLibrary {
public:
    // Builds the procedural meshes ("sphere","cube","plane","torus") and
    // texture sets ("tile","rust"). `projectRoot` anchors relative model paths.
    void init(const std::string& projectRoot);
    void destroy();

    const std::string& projectRoot() const { return root_; }

    Mesh* mesh(const std::string& name);
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

    std::string resolvePath(const std::string& maybeRelative) const;
    std::string toProjectRelative(const std::string& absolute) const;

private:
    std::string root_;
    std::map<std::string, Mesh> meshes_;
    std::map<std::string, MaterialTextures> texSets_;
    std::map<std::string, std::unique_ptr<Model>> models_; // key: normalized relative path
    std::map<std::string, std::unique_ptr<MaterialGraphAsset>> matGraphs_;
    std::map<std::string, std::unique_ptr<DataTable>> dataTables_;
};

} // namespace ae
