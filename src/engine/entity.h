// Aether Engine — an actor in the world: a named node with a transform, a
// parent/child hierarchy, and a bag of components.
#pragma once
#include "transform.h"
#include "component.h"
#include "../core/guid.h"
#include <memory>
#include <string>
#include <vector>
#include <cstdint>

namespace ae {

class World;

class Entity {
public:
    Transform transform;

    const std::string& name() const { return name_; }
    void setName(const std::string& n) { name_ = n; }
    // Per-session handle (fast, not persisted). Use guid() to reference an
    // entity durably (across save/load) — see core/guid.h and EntityRef below.
    uint32_t id() const { return id_; }
    const Guid& guid() const { return guid_; }
    World& world() const { return *world_; }

    bool active() const { return active_; }
    void setActive(bool a) { active_ = a; }

    Entity* parent() const { return parent_; }
    const std::vector<Entity*>& children() const { return children_; }

    // World-space matrix, valid after World::update recomputes the hierarchy.
    const Mat4& worldMatrix() const { return world_matrix_; }
    Vec3 worldPosition() const {
        return Vec3(world_matrix_.m[3][0], world_matrix_.m[3][1], world_matrix_.m[3][2]);
    }

    template <typename T, typename... Args>
    T* addComponent(Args&&... args) {
        auto c = std::make_unique<T>(std::forward<Args>(args)...);
        c->owner_ = this;
        c->world_ = world_;
        T* raw = c.get();
        components_.push_back(std::move(c));
        return raw;
    }

    template <typename T>
    T* getComponent() const {
        for (const auto& c : components_)
            if (T* t = dynamic_cast<T*>(c.get())) return t;
        return nullptr;
    }

    // For the editor inspector.
    const std::vector<std::unique_ptr<Component>>& components() const { return components_; }
    void removeComponent(Component* c) {
        for (auto it = components_.begin(); it != components_.end(); ++it)
            if (it->get() == c) { components_.erase(it); return; }
    }
    int depth() const {
        int d = 0;
        for (Entity* p = parent_; p; p = p->parent_) ++d;
        return d;
    }

private:
    friend class World;
    std::string name_;
    uint32_t id_ = 0;
    Guid guid_;
    World* world_ = nullptr;
    Entity* parent_ = nullptr;
    std::vector<Entity*> children_;
    std::vector<std::unique_ptr<Component>> components_;
    Mat4 world_matrix_ = Mat4::identity();
    bool active_ = true;
    bool pendingDestroy_ = false;
};

// A durable, serializable reference to an entity. Stores the target's Guid and
// caches the resolved pointer, re-resolving through the World whenever the
// cache is stale (target destroyed, or a freshly-loaded scene). This is what
// component fields and editor dropdowns hold instead of a raw Entity*.
struct EntityRef {
    Guid guid;
    mutable Entity* cached = nullptr; // re-resolved lazily; get() is logically const

    void set(Entity* e) {
        cached = e;
        guid = e ? e->guid() : Guid{};
    }
    void clear() {
        cached = nullptr;
        guid = Guid{};
    }
    bool valid() const { return guid.valid(); }

    // Resolves (and caches) via the World; nullptr if the target is gone.
    Entity* get(const World& world) const;
};

} // namespace ae
