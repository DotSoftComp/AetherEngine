// Aether Engine — glTF 2.0 loader (.glb and .gltf), PBR metallic-roughness.
// Supports: meshes (POSITION/NORMAL/TANGENT/TEXCOORD_0/JOINTS_0/WEIGHTS_0,
// u8/u16/u32 indices), morph targets (POSITION/NORMAL deltas + weights
// animation), full material texture set, node hierarchies (TRS + matrix),
// skins with inverse bind matrices, animation clips (LINEAR/STEP, CUBICSPLINE
// sampled at keys), buffers from GLB chunks, external files and base64 data
// URIs. Images decoded via WIC.
//
// Animation is fully instanced: a Model is an immutable shared asset, and all
// sampling writes into a caller-owned ModelPose (node-local TRS, world
// matrices, joint palettes, morph weights + morphed per-instance meshes). Any
// number of entities can share one Model and animate independently.
#pragma once
#include "mesh.h"
#include "texture.h"
#include "../scene/scene.h"
#include <map>
#include <string>
#include <vector>

namespace ae {

// Per-instance animation state for a shared Model. Owned by ModelComponent;
// initialized with Model::initPose and written by the const sampling API.
// Non-copyable (morphed meshes own GPU buffers); movable.
struct ModelPose {
    // One node's local TRS — the unit of pose blending.
    struct NodeTRS {
        Vec3 t{0, 0, 0};
        Vec4 r{0, 0, 0, 1};
        Vec3 s{1, 1, 1};
    };
    std::vector<NodeTRS> locals;      // node-local transforms
    std::vector<uint8_t> useMatrix;   // node still poses via its bind matrix
    std::vector<Mat4> worlds;         // model-space node matrices
    std::vector<std::vector<Mat4>> palettes; // per skin: joint matrices (GPU)
    std::map<int, std::vector<float>> morphWeights; // node -> per-target weights

    // Lazily-created per-instance GPU meshes for primitives with morph
    // targets (CPU-blended, re-uploaded when weights change).
    struct MorphMesh {
        Mesh mesh;
        bool created = false;
        std::vector<float> lastWeights;
    };
    std::map<int, MorphMesh> morphMeshes; // key: Model mesh index

    ModelPose() = default;
    ModelPose(const ModelPose&) = delete;
    ModelPose& operator=(const ModelPose&) = delete;
    ModelPose(ModelPose&&) = default;
    ModelPose& operator=(ModelPose&&) = default;
    ~ModelPose() { destroyGpu(); }

    bool empty() const { return locals.empty(); }
    void destroyGpu() {
        for (auto& [i, m] : morphMeshes)
            if (m.created) m.mesh.destroy();
        morphMeshes.clear();
    }
};

class Model {
public:
    // Dispatches on extension: .glb/.gltf (full: skins + clips + morphs),
    // .obj (static, hand-written parser + MTL materials), .fbx (static, via
    // vendored ufbx).
    bool load(const char* path);
    void destroy();

    int clipCount() const { return (int)clips_.size(); }
    const char* clipName(int i) const { return clips_[i].name.c_str(); }
    int clipIndex(const std::string& name) const;
    float clipDuration(int i) const;

    // ---- per-instance posing (const; writes into a caller-owned pose) ----
    // Sizes the pose and fills it with the bind pose (worlds + palettes valid).
    void initPose(ModelPose& p) const;
    // Locals (and morph weights) back to bind; worlds left stale.
    void resetPoseLocals(ModelPose& p) const;
    // Overwrites the channels `clip` animates at clip-local time t (seconds,
    // caller handles looping). Leaves worlds stale — call finalizePose after.
    void evalClip(int clip, float t, ModelPose& p) const;
    // Recomputes worlds + joint palettes, and re-blends/re-uploads any morphed
    // meshes whose weights changed. Call once per frame after sampling.
    void finalizePose(ModelPose& p) const;
    // dst = lerp(dst, src, w) per node (nlerp for rotations, lerp for T/S and
    // morph weights). `mask` (per node, from subtreeMask) scales w per node.
    static void blendPose(ModelPose& dst, const ModelPose& src, float w,
                          const std::vector<float>* mask = nullptr);

    // ---- skeleton queries (bone masks, root motion, IK) ----
    int nodeCount() const { return (int)nodes_.size(); }
    int nodeIndex(const std::string& name) const;
    int nodeParent(int i) const { return nodes_[i].parent; }
    const char* nodeName(int i) const { return nodes_[i].name.c_str(); }
    // Per-node weights: 1 inside the subtree rooted at the named node, else 0.
    // Empty result if the bone is unknown.
    std::vector<float> subtreeMask(const std::string& bone) const;
    // Samples only `node`'s translation channel of `clip` at time t. False if
    // the clip has no translation channel for that node.
    bool clipTranslation(int clip, int node, float t, Vec3& out) const;
    // Heuristic root-motion bone: the shallowest node a clip translates.
    int guessRootBone() const;

    // Appends one Renderable per mesh-primitive. `pose` selects the instance
    // pose (worlds/palettes/morphed meshes); null renders the bind pose. The
    // Model and pose must outlive the RenderScene (Renderables point into
    // their meshes and palettes).
    void emit(RenderScene& out, const Mat4& base, const ModelPose* pose = nullptr) const;

    // Appends world-space bind-pose triangles (into the flat `verts`/`tris`
    // Recast layout) for navmesh baking. `base` is the owning entity's world
    // matrix. Uses retained CPU geometry (keepNavGeo at load).
    void collectNavTriangles(const Mat4& base, std::vector<float>& verts,
                             std::vector<int>& tris) const;

    // Local-space AABB of the bind pose (for framing / bounds).
    Vec3 boundsMin() const { return boundsMin_; }
    Vec3 boundsMax() const { return boundsMax_; }

private:
    struct Node {
        int parent = -1;
        std::string name;
        Vec3 t{0, 0, 0};
        Vec4 r{0, 0, 0, 1}; // quaternion
        Vec3 s{1, 1, 1};
        bool hasMatrix = false;
        Mat4 matrix = Mat4::identity();
        Mat4 world = Mat4::identity(); // bind-pose world (pose-less rendering)
        std::vector<int> children;
    };
    struct Skin {
        std::vector<int> joints;   // node indices
        std::vector<Mat4> inverseBind;
        std::vector<Mat4> palette; // bind-pose palette (pose-less rendering)
    };
    struct Channel {
        int node = -1;
        int path = 0;   // 0 = translation, 1 = rotation, 2 = scale, 3 = weights
        int interp = 0; // 0 = LINEAR, 1 = STEP, 2 = CUBICSPLINE
        int comp = 3;   // floats per key value (3/4, or morph-target count)
        std::vector<float> times;
        std::vector<float> values;
    };
    struct Clip {
        std::string name;
        float duration = 0;
        std::vector<Channel> channels;
    };
    struct Draw {
        int node = -1;
        int mesh = -1;      // index into meshes_
        int material = -1;
        int skin = -1;      // index into skins_
    };
    // CPU-side morph data for one engine mesh (primitive with "targets").
    struct MorphData {
        MeshData base; // bind vertices (re-blended into per-instance meshes)
        struct Target {
            std::vector<Vec3> dpos, dnrm; // per-vertex deltas (dnrm may be empty)
        };
        std::vector<Target> targets;
    };

    void computeWorlds();
    // Sample one channel at time t into out[comp] floats.
    static void sampleChannel(const Channel& ch, float t, float* out);
    bool loadGltf(const char* path);
    bool loadObj(const char* path);  // model_obj.cpp
    bool loadFbx(const char* path);  // model_fbx.cpp (ufbx)
    friend struct ModelBuild;        // importer-side access to the internals

    std::vector<Mesh> meshes_;
    std::vector<Material> materials_;
    std::vector<Texture2D> textures_;
    std::vector<Node> nodes_;
    std::vector<int> roots_;
    std::vector<Skin> skins_;
    std::vector<Clip> clips_;
    std::vector<Draw> draws_;
    std::map<int, MorphData> morphs_;             // engine mesh index -> targets
    std::map<int, std::vector<float>> morphDefaults_; // node -> default weights
    Vec3 boundsMin_{0, 0, 0}, boundsMax_{0, 0, 0};
};

} // namespace ae
