// Aether Engine — base class for everything you attach to an Entity.
//
// Components have a lifecycle (onStart once, onUpdate each frame) and may
// contribute draws/lights to the per-frame RenderScene. Gameplay logic lives
// in Behaviors (a semantic alias below) that override onUpdate — this is where
// the Detroit-style choice/dialogue scripting will hook in.
#pragma once

namespace ae {

class Entity;
class World;
struct RenderScene;
class PropertyVisitor;
class AssetLibrary;

class Component {
public:
    virtual ~Component() = default;

    // Called once, the first frame after the component is attached and active.
    virtual void onStart() {}
    // Called every frame while the owning entity is active.
    virtual void onUpdate(float dt) { (void)dt; }
    // Called after every onUpdate and the physics step, before rendering —
    // for pose post-processing (IK) and anything that must see final
    // transforms regardless of component order.
    virtual void onLateUpdate(float dt) { (void)dt; }
    // Called during render-packet assembly; append draws/lights here.
    virtual void contribute(RenderScene& out) { (void)out; }

    // ---- reflection (see engine/reflect.h) ----
    // Stable serialized type name; must match the ComponentRegistry entry.
    // "" = not serializable (the component silently skips save/clone).
    virtual const char* typeName() const { return ""; }
    // Visit every serializable field once. Drives save/load/clone/inspector.
    virtual void reflect(PropertyVisitor& v) { (void)v; }
    // Post-deserialize fixup: resolve asset names/paths to live pointers.
    // Also invoked by the editor after an asset-typed property changes.
    virtual void onDeserialized(AssetLibrary& assets) { (void)assets; }

    Entity& entity() const { return *owner_; }
    World& world() const { return *world_; }

private:
    friend class Entity;
    Entity* owner_ = nullptr;
    World* world_ = nullptr;
    bool started_ = false;
    friend class World;
};

// Gameplay scripts derive from Behavior purely for readability; it adds nothing
// beyond Component's overridable onStart/onUpdate.
class Behavior : public Component {};

} // namespace ae
