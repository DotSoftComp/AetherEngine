// Aether Engine — material graphs: artist-authored shading without code.
//
// A MaterialGraph is a pure-dataflow node graph (JSON in assets/materials/)
// whose Output node feeds the PBR inputs — BaseColor, Metallic, Roughness,
// Emissive, Opacity, AO. The engine GENERATES GLSL from the graph and compiles
// it as a variant of the standard pbr.frag (the MATERIAL_GRAPH block), so
// graph materials get the full lighting pipeline — sun + local lights,
// shadows, IBL, SSAO, fog, bloom — for free, in both the deferred and forward
// (blended) passes.
//
// Node types are SELF-DESCRIBING: one registry entry each (pins, params, and a
// GLSL-emitting lambda) drives the codegen AND the canvas editor
// (editor/material_graph_panel). Adding a node = one small registration.
//
// Value types are Float / Vec2 / Vec3 with implicit conversions (splat, .x,
// (xy,0)) so wiring "just works". Assign a graph to a MeshRenderer through the
// material's "Material Graph" field (drag a .json from the Content Browser).
#pragma once
#include "shader.h"
#include "texture.h"
#include "../core/math3d.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ae {

enum class MGType { F1, F2, F3 };

struct MGNode {
    std::string id;
    std::string type;
    std::string p;        // string param (texture path)
    float n = 0.0f;       // number param (scalar value / power / sRGB / blend)
    Vec3 v{0, 0, 0};      // vector param (color / tiling / speed)
    std::vector<std::string> in; // per input pin: source node id ("" = default)
    float x = 0.0f, y = 0.0f;    // editor canvas position
};

struct MaterialGraph {
    std::vector<MGNode> nodes;
    int indexOf(const std::string& id) const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].id == id) return (int)i;
        return -1;
    }
    int outputNode() const {
        for (size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i].type == "Output") return (int)i;
        return -1;
    }
};

bool loadMaterialGraph(MaterialGraph& g, const std::string& path);
bool saveMaterialGraph(const MaterialGraph& g, const std::string& path);

// ---- node registry -----------------------------------------------------------
struct MGEmitCtx; // codegen context (defined in the .cpp)

struct MGPinDef {
    std::string name;
    MGType type = MGType::F3;
    std::string defaultExpr; // GLSL used when the pin is unconnected
};

struct MGNodeDef {
    std::string type;
    std::string category;
    std::vector<MGPinDef> in;
    MGType out = MGType::F3;
    const char* pLabel = nullptr; // instance param labels (nullptr = unused)
    const char* nLabel = nullptr;
    const char* vLabel = nullptr;
    bool vIsColor = false; // v edits as a color picker
    // Emits the node's GLSL expression. inExpr[i] is already converted to the
    // declared pin type; use ctx for texture-slot allocation.
    std::function<std::string(const MGNode&, const std::vector<std::string>& inExpr,
                              MGEmitCtx& ctx)>
        emit;
};

const std::vector<MGNodeDef>& materialNodeDefs();
const MGNodeDef* materialNodeDef(const std::string& type);
const char* mgTypeName(MGType t);
unsigned mgTypeColor(MGType t); // editor pin color

// ---- compiled asset ------------------------------------------------------------
// One per graph file, cached by the AssetLibrary. Recompiled in place on save
// (hot reload), so Material::graph pointers stay valid.
struct MaterialGraphAsset {
    MaterialGraph graph;
    Shader shader;                       // pbr variant with the generated block
    std::vector<Texture2D> textures;     // owned Texture nodes' images
    bool blend = false;                  // Output node's "Transparent" flag
    bool valid = false;
    std::string path;                    // absolute source path (for reload)

    // (Re)generates GLSL, compiles the shader, loads textures. Requires a GL
    // context. Returns valid.
    bool compile(const std::string& absPath);
};

// Generates just the GLSL block (exposed for the editor's "view code" and for
// tests). Returns false when the graph has no Output node or a cycle.
bool generateMaterialGLSL(const MaterialGraph& g, std::string& declsOut, std::string& codeOut,
                          std::vector<std::pair<std::string, bool>>& texturesOut /*path,srgb*/);

} // namespace ae
