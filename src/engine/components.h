// Aether Engine — built-in components you attach to entities.
#pragma once
#include "entity.h"
#include "world.h"
#include "reflect.h"
#include "assets.h"
#include "../render/mesh.h"
#include "../render/gltf.h"
#include "../scene/scene.h"

namespace ae {

// Field order defines the scene-file JSON layout — keep it stable.
inline void reflectMaterial(PropertyVisitor& v, Material& m) {
    v.visit("baseColor", m.baseColor, {PropKind::Color, "Base Color"});
    v.visit("metallic", m.metallic, {PropKind::SliderNorm, "Metallic"});
    v.visit("roughness", m.roughness, {PropKind::SliderNorm, "Roughness"});
    v.visit("emissive", m.emissive, {PropKind::HdrColor, "Emissive"});
    v.visit("uvScale", m.uvScale, {PropKind::Default, "UV Scale", 0.02f, 0.05f, 32.0f});
    v.visit("normalScale", m.normalScale, {PropKind::Default, "Normal Scale", 0.02f});
    v.visit("occlusionStrength", m.occlusionStrength, {PropKind::SliderNorm, "Occlusion"});
    v.visit("alphaCutoff", m.alphaCutoff, {PropKind::Default, "Alpha Cutoff", 0.01f});
    v.visit("doubleSided", m.doubleSided, {PropKind::Default, "Double-sided"});
    v.visit("blend", m.blend, {PropKind::Default, "Transparent (blend)"});
    v.visit("graph", m.graphPath, {PropKind::MaterialGraphPath, "Material Graph"});
    v.visit("subsurface", m.subsurface, {PropKind::SliderNorm, "Subsurface"});
    v.visit("sssCurvature", m.sssCurvature, {PropKind::SliderNorm, "SSS Curvature"});
    v.visit("translucency", m.translucency, {PropKind::SliderNorm, "Translucency"});
    v.visit("sssTint", m.sssTint, {PropKind::Color, "SSS Tint"});
}

// Draws a single mesh with a material at the entity's world transform.
class MeshRenderer : public Component {
public:
    const Mesh* mesh = nullptr;
    Material material;
    bool castShadow = true;
    float drawDistance = 0.0f; // cull beyond this camera distance (0 = never)

    // Asset identity for serialization + the editor's asset combos: the
    // AssetLibrary name this mesh/texture-set came from ("" = unknown).
    std::string meshName;
    std::string texSetName;

    MeshRenderer() = default;
    MeshRenderer(const Mesh* m, const Material& mat, bool shadow = true)
        : mesh(m), material(mat), castShadow(shadow) {}

    const char* typeName() const override { return "MeshRenderer"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("mesh", meshName, {PropKind::MeshName, "Mesh"});
        v.visit("texSet", texSetName, {PropKind::TexSetName, "Textures"});
        v.visit("castShadow", castShadow, {PropKind::Default, "Cast shadow"});
        v.visit("drawDistance", drawDistance, {PropKind::Default, "Draw distance (0 = inf)", 0.5f, 0.0f, 10000.0f});
        v.beginGroup("material");
        reflectMaterial(v, material);
        v.endGroup();
    }
    void onDeserialized(AssetLibrary& assets) override {
        mesh = meshName.empty() ? mesh : assets.mesh(meshName);
        if (!texSetName.empty()) {
            if (const MaterialTextures* t = assets.textureSet(texSetName))
                material.setTextures(*t);
        } else {
            material.albedoTex = material.normalTex = 0;
            material.mrTex = material.occlusionTex = 0;
        }
        material.graph =
            material.graphPath.empty() ? nullptr : assets.materialGraph(material.graphPath);
    }

    void contribute(RenderScene& out) override {
        if (!mesh) return;
        Renderable r;
        r.mesh = mesh;
        r.material = material;
        r.transform = entity().worldMatrix();
        r.castShadow = castShadow;
        r.boundsMin = mesh->boundsMin();
        r.boundsMax = mesh->boundsMax();
        r.maxDistance = drawDistance;
        out.entities.push_back(r);
    }
};

// A light at the entity's world origin. Point (omni), Spot (cone aimed down the
// entity's local -Z, like a camera), or Directional (parallel rays along -Z,
// position ignored). Spot/directional aim follows the entity's rotation, so a
// gizmo rotation directs the beam — ideal for cinematic key/rim lighting.
class LightComponent : public Component {
public:
    int type = 0;              // 0 = point, 1 = spot, 2 = directional
    Vec3 color{1, 1, 1};
    float intensity = 1.0f;
    float range = 12.0f;       // point/spot falloff distance
    float innerAngle = 25.0f;  // spot: full-bright cone half-angle (degrees)
    float outerAngle = 35.0f;  // spot: cone edge half-angle (degrees)

    LightComponent() = default;
    LightComponent(const Vec3& c, float i) : color(c), intensity(i) {}

    const char* typeName() const override { return "Light"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("lightType", type, {PropKind::Default, "Type (0=Point 1=Spot 2=Dir)", 1, 0, 2});
        v.visit("color", color, {PropKind::Radiance, "Color (radiance)", 0.1f, 0.0f, 200.0f});
        v.visit("intensity", intensity, {PropKind::Default, "Intensity", 0.02f, 0.0f, 100.0f});
        v.visit("range", range, {PropKind::Default, "Range", 0.1f, 0.0f, 500.0f});
        v.visit("innerAngle", innerAngle, {PropKind::Angle, "Spot inner", 0.5f, 0.0f, 89.0f});
        v.visit("outerAngle", outerAngle, {PropKind::Angle, "Spot outer", 0.5f, 0.0f, 89.0f});
    }

    void contribute(RenderScene& out) override {
        Mat4 m = entity().worldMatrix();
        Light l;
        l.position = entity().worldPosition();
        l.color = color * intensity;
        l.direction = normalize(Vec3(-m.m[2][0], -m.m[2][1], -m.m[2][2])); // local -Z beam axis
        l.type = type;
        l.range = range;
        float inner = innerAngle, outer = outerAngle < innerAngle ? innerAngle : outerAngle;
        l.cosInner = std::cos(radians(inner));
        l.cosOuter = std::cos(radians(outer));
        out.lights.push_back(l);
    }
};

// Turns an entity into a camera: pose (position/forward/up) comes entirely
// from the entity's world transform, so ANY way of moving/orienting an entity
// (a fixed placement, CameraController free-fly, a third-person/follow rig,
// or a dialogue cutscene) works as a camera without this component knowing
// which. Multiple CameraComponents can coexist in a scene — each submits its
// pose as a named shot; World's camera director (see World::requestCamera)
// picks which one is actually active and blends between cuts. Set isDefault
// on the one camera that should be used when nothing else is requested (the
// ordinary gameplay camera).
class CameraComponent : public Component {
public:
    float fovY = 55.0f;
    float zNear = 0.1f;
    float zFar = 300.0f;
    bool isDefault = false;

    const char* typeName() const override { return "Camera"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("fov", fovY, {PropKind::Angle, "FOV", 0.5f, 10.0f, 120.0f});
        v.visit("near", zNear, {PropKind::Default, "Near", 0.01f, 0.01f, 10.0f});
        v.visit("far", zFar, {PropKind::Default, "Far", 1.0f, 10.0f, 5000.0f});
        // Exclusive flag: the inspector clears it on every other camera; a
        // duplicate must never steal it (skipOnClone).
        v.visit("isDefault", isDefault,
                {PropKind::CameraDefaultFlag, "Default gameplay camera", 0, 0, 0, true});
    }

    void contribute(RenderScene&) override {
        Mat4 m = entity().worldMatrix();
        Camera cam;
        cam.position = entity().worldPosition();
        cam.forwardDir = normalize(Vec3(-m.m[2][0], -m.m[2][1], -m.m[2][2])); // local -Z
        cam.upDir = normalize(Vec3(m.m[1][0], m.m[1][1], m.m[1][2]));        // local +Y
        cam.fovY = fovY;
        cam.zNear = zNear;
        cam.zFar = zFar;
        world().submitCameraShot(entity().name(), cam, isDefault);
    }
};

// Renders (and optionally animates) a glTF model at the entity's transform.
// The Model is owned elsewhere and must outlive the world.
class ModelComponent : public Component {
public:
    Model* model = nullptr;
    bool animate = true;

    // Project-relative asset path for serialization (resolved via AssetLibrary).
    std::string modelPath;

    ModelComponent() = default;
    explicit ModelComponent(Model* m, bool anim = true) : model(m), animate(anim) {}

    const char* typeName() const override { return "Model"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("path", modelPath, {PropKind::ModelPath, "Model"});
        v.visit("animate", animate, {PropKind::Default, "Animate"});
    }
    void onDeserialized(AssetLibrary& assets) override {
        model = modelPath.empty() ? nullptr : assets.model(modelPath);
    }

    void onUpdate(float) override {
        if (model && animate) model->sample(world().time());
    }
    void contribute(RenderScene& out) override {
        if (model) model->emit(out, entity().worldMatrix());
    }
};

} // namespace ae
