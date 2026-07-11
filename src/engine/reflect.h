// Aether Engine — component property reflection.
//
// Each component implements `void reflect(PropertyVisitor&)`, visiting every
// serializable field once with its JSON key. That single function drives:
//   - scene save (JsonWriteVisitor)            engine/scene_io.cpp
//   - scene load (JsonReadVisitor)             engine/scene_io.cpp
//   - duplicate/prefab clone (write→read)      engine/scene_io.cpp
//   - prefab EntityRef remapping               engine/scene_io.cpp
//   - the Details-panel inspector              editor/inspector_visitor.*
//
// Adding a component (built-in or game-module script) therefore never touches
// the serializer or the editor — implement reflect(), register the type in
// the ComponentRegistry, done. Keys must stay stable: they are the scene file
// format. PropKind/PropMeta carry inspector presentation hints only.
#pragma once
#include "../core/math3d.h"
#include <string>

namespace ae {

struct EntityRef;

enum class PropKind {
    Default,
    SliderNorm,        // float: slider clamped to [min,max] (default 0..1)
    Angle,             // float: degrees, slider when min<max
    Color,             // Vec3/Vec4: LDR color picker
    HdrColor,          // Vec3: HDR color picker
    Radiance,          // Vec3: light radiance drags
    MeshName,          // string: AssetLibrary mesh combo
    TexSetName,        // string: AssetLibrary texture-set combo ("(none)" allowed)
    ModelPath,         // string: read-only asset path
    DialogueScenePath, // string: path input + "Open in Dialogue Graph"
    ScriptGraphPath,   // string: path input + "Open in Script Graph"
    MaterialGraphPath, // string: path input + "Open in Material Graph"
    UIDocPath,         // string: path input + "Open in UI Designer"
    AnimGraphPath,     // string: path input + "Open in Anim Graph"
    AudioClip,         // string: drop-target for a .wav from the Content Browser
    CameraDefaultFlag, // bool: exclusive default-camera checkbox
    Multiline,         // string: multiline text box
};

struct PropMeta {
    PropKind kind = PropKind::Default;
    const char* label = nullptr; // inspector label (defaults to the JSON key)
    float speed = 0.05f;         // drag speed
    float min = 0.0f, max = 0.0f; // min==max means unbounded
    bool skipOnClone = false;    // never copied on duplicate/prefab-instance

    PropMeta() = default;
    PropMeta(PropKind k, const char* lbl = nullptr, float sp = 0.05f, float mn = 0.0f,
             float mx = 0.0f, bool skipClone = false)
        : kind(k), label(lbl), speed(sp), min(mn), max(mx), skipOnClone(skipClone) {}
};

class PropertyVisitor {
public:
    virtual ~PropertyVisitor() = default;

    virtual void visit(const char* key, float& v, const PropMeta& m = {}) = 0;
    virtual void visit(const char* key, int& v, const PropMeta& m = {}) = 0;
    virtual void visit(const char* key, bool& v, const PropMeta& m = {}) = 0;
    virtual void visit(const char* key, std::string& v, const PropMeta& m = {}) = 0;
    virtual void visit(const char* key, Vec3& v, const PropMeta& m = {}) = 0;
    virtual void visit(const char* key, Vec4& v, const PropMeta& m = {}) = 0;

    // Durable entity reference. Serializes as two sibling keys: guidKey (the
    // Guid) and nameKey (a legacy/authoring name fallback kept in sync).
    virtual void visit(const char* guidKey, const char* nameKey, EntityRef& ref,
                       std::string& legacyName, const PropMeta& m = {}) = 0;

    // Nested JSON object (e.g. MeshRenderer's "material" block).
    virtual void beginGroup(const char* key) { (void)key; }
    virtual void endGroup() {}
};

} // namespace ae
