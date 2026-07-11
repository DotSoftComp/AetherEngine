// Aether Engine — glTF 2.0 loader (.glb and .gltf), PBR metallic-roughness.
// Supports: meshes (POSITION/NORMAL/TANGENT/TEXCOORD_0/JOINTS_0/WEIGHTS_0,
// u8/u16/u32 indices), full material texture set, node hierarchies (TRS +
// matrix), skins with inverse bind matrices, animation clips (LINEAR/STEP,
// CUBICSPLINE sampled at keys), buffers from GLB chunks, external files and
// base64 data URIs. Images decoded via WIC.
#pragma once
#include "mesh.h"
#include "texture.h"
#include "../scene/scene.h"
#include <string>
#include <vector>

namespace ae {

class Model {
public:
    // Dispatches on extension: .glb/.gltf (full: skins + clips), .obj (static,
    // hand-written parser + MTL materials), .fbx (static, via vendored ufbx).
    bool load(const char* path);
    void destroy();

    int clipCount() const { return (int)clips_.size(); }
    const char* clipName(int i) const { return clips_[i].name.c_str(); }
    void setClip(int index) { activeClip_ = index; }
    int clipIndex(const std::string& name) const;
    float clipDuration(int i) const;

    // Samples the active clip at `time` (seconds, looped) and recomputes node
    // world matrices + joint palettes. Call once per frame before emit().
    void sample(float time);

    // Animator API: pose one clip at an explicit local time, or crossfade two
    // clips (w = 0 -> all A, 1 -> all B). Times are clip-local seconds
    // (caller handles looping); recomputes worlds + palettes like sample().
    void sampleClipTime(int clip, float t);
    void sampleBlended(int clipA, float tA, int clipB, float tB, float w);

    // Appends one Renderable per mesh-primitive using the current pose,
    // transformed by `base` (the owning entity's world matrix). The Model must
    // outlive the RenderScene (Renderables point into its meshes and palettes).
    void emit(RenderScene& out, const Mat4& base) const;

    // Local-space AABB of the bind pose (for framing / bounds).
    Vec3 boundsMin() const { return boundsMin_; }
    Vec3 boundsMax() const { return boundsMax_; }

private:
    struct Node {
        int parent = -1;
        Vec3 t{0, 0, 0};
        Vec4 r{0, 0, 0, 1}; // quaternion
        Vec3 s{1, 1, 1};
        bool hasMatrix = false;
        Mat4 matrix = Mat4::identity();
        Mat4 world = Mat4::identity();
        std::vector<int> children;
    };
    struct Skin {
        std::vector<int> joints;   // node indices
        std::vector<Mat4> inverseBind;
        std::vector<Mat4> palette; // model-space joint matrices (GPU upload)
    };
    struct Channel {
        int node = -1;
        int path = 0;   // 0 = translation, 1 = rotation, 2 = scale
        int interp = 0; // 0 = LINEAR, 1 = STEP, 2 = CUBICSPLINE
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

    void computeWorlds();
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
    Vec3 boundsMin_{0, 0, 0}, boundsMax_{0, 0, 0};
    int activeClip_ = 0;
};

} // namespace ae
