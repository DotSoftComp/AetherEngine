// Aether Engine — Jolt Physics backend for PhysicsWorld (see physics.h).
//
// Each RigidBody/Collider pair is mirrored as a Jolt body. Every step we:
//   1. reconcile bodies with the live components (create new, drop dead),
//   2. push authored/gameplay state in (kinematic poses, gameplay velocities),
//   3. PhysicsSystem::Update,
//   4. read dynamic poses + velocities + grounded/overlap flags back out.
#include "physics.h"
#include "physics_components.h"
#include "../engine/world.h"
#include "../engine/entity.h"
#include "../core/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Body/BodyFilter.h>

#include <cmath>
#include <cstdarg>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace ae {

namespace {

// ---- Jolt collision layers (two-layer setup: moving vs. non-moving) --------
namespace Layers {
static constexpr JPH::ObjectLayer NON_MOVING = 0;
static constexpr JPH::ObjectLayer MOVING = 1;
static constexpr JPH::ObjectLayer NUM = 2;
}
namespace BPLayers {
static constexpr JPH::BroadPhaseLayer NON_MOVING(0);
static constexpr JPH::BroadPhaseLayer MOVING(1);
static constexpr JPH::uint NUM = 2;
}

class BPLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterface() {
        map_[Layers::NON_MOVING] = BPLayers::NON_MOVING;
        map_[Layers::MOVING] = BPLayers::MOVING;
    }
    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer l) const override { return map_[l]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "layer"; }
#endif
private:
    JPH::BroadPhaseLayer map_[Layers::NUM];
};

class ObjectVsBPFilter final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l1, JPH::BroadPhaseLayer l2) const override {
        if (l1 == Layers::NON_MOVING) return l2 == BPLayers::MOVING;
        return true; // MOVING collides with everything
    }
};

class ObjectPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer l1, JPH::ObjectLayer l2) const override {
        if (l1 == Layers::NON_MOVING) return l2 == Layers::MOVING;
        return true;
    }
};

// Records, per body index, whether it had a near-horizontal contact this step
// (grounded) and any contact (overlap). Callbacks run on job threads → guarded.
class ContactRecorder final : public JPH::ContactListener {
public:
    void reset() {
        std::lock_guard<std::mutex> lk(m_);
        grounded_.clear();
        overlap_.clear();
    }
    bool grounded(JPH::uint32 idx) const {
        std::lock_guard<std::mutex> lk(m_);
        return grounded_.count(idx) != 0;
    }
    bool overlap(JPH::uint32 idx) const {
        std::lock_guard<std::mutex> lk(m_);
        return overlap_.count(idx) != 0;
    }
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings&) override { note(b1, b2, m); }
    void OnContactPersisted(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                            JPH::ContactSettings&) override { note(b1, b2, m); }

private:
    void note(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m) {
        bool vertical = std::fabs(m.mWorldSpaceNormal.GetY()) > 0.7f;
        std::lock_guard<std::mutex> lk(m_);
        JPH::uint32 i1 = b1.GetID().GetIndex(), i2 = b2.GetID().GetIndex();
        overlap_.insert(i1);
        overlap_.insert(i2);
        if (vertical) { grounded_.insert(i1); grounded_.insert(i2); }
    }
    mutable std::mutex m_;
    std::unordered_set<JPH::uint32> grounded_;
    std::unordered_set<JPH::uint32> overlap_;
};

void traceImpl(const char* fmt, ...) {
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    AE_LOG("[Jolt] %s", buf);
}

void ensureJoltGlobals() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = traceImpl;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

// ---- conversions -----------------------------------------------------------
JPH::Vec3 toJ(const Vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
JPH::RVec3 toJR(const Vec3& v) { return JPH::RVec3(v.x, v.y, v.z); }
JPH::Quat toJQ(const Vec4& q) { return JPH::Quat(q.x, q.y, q.z, q.w); }
Vec3 fromJ(JPH::Vec3Arg v) { return Vec3(v.GetX(), v.GetY(), v.GetZ()); }
Vec3 fromJR(JPH::RVec3Arg v) { return Vec3((float)v.GetX(), (float)v.GetY(), (float)v.GetZ()); }
Vec4 fromJQ(JPH::QuatArg q) { return Vec4(q.GetX(), q.GetY(), q.GetZ(), q.GetW()); }

Vec3 col(const Mat4& m, int c) { return Vec3(m.m[c][0], m.m[c][1], m.m[c][2]); }
Vec3 mat4Scale(const Mat4& m) { return Vec3(length(col(m, 0)), length(col(m, 1)), length(col(m, 2))); }
Vec4 mat4Rotation(const Mat4& m) {
    Vec3 s = mat4Scale(m);
    Vec3 x = s.x > 1e-8f ? col(m, 0) / s.x : Vec3(1, 0, 0);
    Vec3 y = s.y > 1e-8f ? col(m, 1) / s.y : Vec3(0, 1, 0);
    Vec3 z = s.z > 1e-8f ? col(m, 2) / s.z : Vec3(0, 0, 1);
    return quatFromBasis(x, y, z);
}
Vec3 transformPoint(const Mat4& m, const Vec3& p) {
    Vec4 r = m * Vec4(p, 1.0f);
    return Vec3(r.x, r.y, r.z);
}

// Snapshot of the parameters that define a body's shape/mass; a change forces a
// rebuild (e.g. the collider or scale was edited).
struct Desc {
    int shape = 0, motion = 0;
    Vec3 half{0, 0, 0}, center{0, 0, 0};
    float radius = 0, capHalf = 0, mass = 0;
    bool trigger = false, lockRot = false, gravity = true;
    float rest = 0, fric = 0, linDamp = 0, angDamp = 0;
    bool operator==(const Desc& o) const {
        auto v = [](const Vec3& a, const Vec3& b) {
            return std::fabs(a.x - b.x) + std::fabs(a.y - b.y) + std::fabs(a.z - b.z) < 1e-4f;
        };
        auto f = [](float a, float b) { return std::fabs(a - b) < 1e-4f; };
        return shape == o.shape && motion == o.motion && v(half, o.half) && v(center, o.center) &&
               f(radius, o.radius) && f(capHalf, o.capHalf) && f(mass, o.mass) &&
               trigger == o.trigger && lockRot == o.lockRot && gravity == o.gravity &&
               f(rest, o.rest) && f(fric, o.fric) && f(linDamp, o.linDamp) && f(angDamp, o.angDamp);
    }
    bool operator!=(const Desc& o) const { return !(*this == o); }
};

struct Rec {
    JPH::BodyID id;
    Guid guid;
    Desc desc;
    bool dynamic = false;
    float centerY = 0.0f;  // collider centre offset (y), for the grounded probe
    float halfY = 0.5f;    // half-height of the shape, for the grounded probe
    Vec3 lastPos{0, 0, 0};
    Vec4 lastRot{0, 0, 0, 1};
};

float shapeHalfY(const Desc& d) {
    if (d.shape == (int)ColliderShape::Sphere) return d.radius;
    if (d.shape == (int)ColliderShape::Capsule) return d.capHalf + d.radius;
    return d.half.y;
}

JPH::ShapeRefC makeShape(const Desc& d) {
    JPH::ShapeRefC inner;
    if (d.shape == (int)ColliderShape::Sphere) {
        inner = new JPH::SphereShape(std::max(0.02f, d.radius));
    } else if (d.shape == (int)ColliderShape::Capsule && d.capHalf > 1e-3f) {
        inner = new JPH::CapsuleShape(d.capHalf, std::max(0.02f, d.radius));
    } else if (d.shape == (int)ColliderShape::Capsule) {
        inner = new JPH::SphereShape(std::max(0.02f, d.radius));
    } else {
        JPH::Vec3 h(std::max(0.02f, d.half.x), std::max(0.02f, d.half.y), std::max(0.02f, d.half.z));
        inner = new JPH::BoxShape(h);
    }
    if (std::fabs(d.center.x) + std::fabs(d.center.y) + std::fabs(d.center.z) > 1e-5f) {
        JPH::RotatedTranslatedShapeSettings off(toJ(d.center), JPH::Quat::sIdentity(), inner);
        JPH::ShapeSettings::ShapeResult r = off.Create();
        if (r.IsValid()) return r.Get();
    }
    return inner;
}

Desc describe(Entity* e, ColliderComponent* c, RigidBodyComponent* rb) {
    Desc d;
    Vec3 scale = mat4Scale(e->worldMatrix());
    float rScale = std::max(scale.x, scale.z);
    d.shape = c->shape;
    d.half = c->halfExtents * scale;
    d.center = c->center * scale;
    d.radius = c->radius * rScale;
    d.capHalf = std::max(0.0f, (c->height * 0.5f - c->radius)) * scale.y;
    d.trigger = c->isTrigger;
    d.motion = rb ? rb->bodyType : (int)BodyType::Static;
    d.mass = rb ? rb->mass : 0.0f;
    d.lockRot = rb ? rb->lockRotation : false;
    d.gravity = rb ? rb->useGravity : true;
    d.rest = rb ? rb->restitution : 0.0f;
    d.fric = rb ? rb->friction : 0.5f;
    d.linDamp = rb ? rb->linearDamping : 0.0f;
    d.angDamp = rb ? rb->angularDamping : 0.0f;
    return d;
}

} // namespace

struct PhysicsWorld::Impl {
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                  (int)std::max(1u, std::thread::hardware_concurrency() - 1)};
    BPLayerInterface bpLayer;
    ObjectVsBPFilter objVsBp;
    ObjectPairFilter objPair;
    JPH::PhysicsSystem system;
    ContactRecorder contacts;
    std::unordered_map<Entity*, Rec> bodies;
    std::unordered_map<JPH::uint32, Entity*> byIndex;

    // NOTE: ensureJoltGlobals() must have run before this object is constructed —
    // members (temp allocator, job system) allocate through Jolt's allocator as
    // they are built, before any constructor body would run. ensure() guarantees
    // it by calling ensureJoltGlobals() prior to `new Impl()`.
    Impl() {
        system.Init(2048, 0, 4096, 2048, bpLayer, objVsBp, objPair);
        system.SetContactListener(&contacts);
    }

    JPH::BodyInterface& bi() { return system.GetBodyInterface(); }

    void removeRec(const Rec& r) {
        bi().RemoveBody(r.id);
        bi().DestroyBody(r.id);
    }

    void clearAll() {
        for (auto& kv : bodies) removeRec(kv.second);
        bodies.clear();
        byIndex.clear();
    }

    ~Impl() { clearAll(); }
};

PhysicsWorld::~PhysicsWorld() { delete impl_; }

PhysicsWorld::Impl* PhysicsWorld::ensure() {
    if (!impl_) {
        ensureJoltGlobals(); // before Impl's allocating members are constructed
        impl_ = new Impl();
    }
    return impl_;
}

void PhysicsWorld::clear() {
    if (impl_) impl_->clearAll();
}

int PhysicsWorld::bodyCount() const {
    return impl_ ? (int)impl_->bodies.size() : 0;
}

void PhysicsWorld::step(World& world, float dt) {
    if (dt <= 0.0f) return;
    Impl* im = ensure();
    JPH::BodyInterface& bi = im->bi();
    im->system.SetGravity(toJ(gravity));

    // 1) Reconcile bodies with the live components.
    std::unordered_set<Entity*> seen;
    for (const auto& up : world.entities()) {
        Entity* e = up.get();
        if (!e->active()) continue;
        auto* colc = e->getComponent<ColliderComponent>();
        if (!colc) continue;
        auto* rb = e->getComponent<RigidBodyComponent>();
        seen.insert(e);
        colc->overlapping = false;
        if (rb && rb->type() == BodyType::Dynamic) rb->grounded = false;

        Desc d = describe(e, colc, rb);
        Vec3 wpos = e->worldPosition();
        Vec4 wrot = mat4Rotation(e->worldMatrix());

        auto it = im->bodies.find(e);
        bool rebuild = it == im->bodies.end() || it->second.guid != e->guid() || it->second.desc != d;
        if (rebuild) {
            if (it != im->bodies.end()) { im->removeRec(it->second); im->byIndex.erase(it->second.id.GetIndex()); im->bodies.erase(it); }

            JPH::EMotionType mt = d.motion == (int)BodyType::Dynamic     ? JPH::EMotionType::Dynamic
                                  : d.motion == (int)BodyType::Kinematic ? JPH::EMotionType::Kinematic
                                                                         : JPH::EMotionType::Static;
            JPH::ObjectLayer layer = mt == JPH::EMotionType::Static ? Layers::NON_MOVING : Layers::MOVING;
            JPH::BodyCreationSettings s(makeShape(d), toJR(wpos), toJQ(wrot), mt, layer);
            s.mRestitution = d.rest;
            s.mFriction = d.fric;
            s.mLinearDamping = d.linDamp;
            s.mAngularDamping = d.angDamp;
            s.mGravityFactor = d.gravity ? 1.0f : 0.0f;
            s.mIsSensor = d.trigger;
            if (mt == JPH::EMotionType::Dynamic) {
                s.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
                s.mMassPropertiesOverride.mMass = std::max(0.001f, d.mass);
                if (d.lockRot)
                    s.mAllowedDOFs = JPH::EAllowedDOFs::TranslationX | JPH::EAllowedDOFs::TranslationY |
                                     JPH::EAllowedDOFs::TranslationZ;
            }
            JPH::BodyID id = bi.CreateAndAddBody(
                s, mt == JPH::EMotionType::Dynamic ? JPH::EActivation::Activate
                                                   : JPH::EActivation::DontActivate);
            Rec r;
            r.id = id;
            r.guid = e->guid();
            r.desc = d;
            r.dynamic = mt == JPH::EMotionType::Dynamic;
            r.centerY = d.center.y;
            r.halfY = shapeHalfY(d);
            r.lastPos = wpos;
            r.lastRot = wrot;
            if (rb) { bi.SetLinearVelocity(id, toJ(rb->velocity)); bi.SetAngularVelocity(id, toJ(rb->angularVelocity)); }
            im->bodies[e] = r;
            im->byIndex[id.GetIndex()] = e;
            continue;
        }

        Rec& r = it->second;
        if (!r.dynamic) {
            // Static/kinematic pose is driven by the entity (editor, behaviors,
            // animation). Push it only when it actually moved.
            bool moved = length(wpos - r.lastPos) > 1e-4f ||
                         std::fabs(wrot.x - r.lastRot.x) + std::fabs(wrot.y - r.lastRot.y) +
                                 std::fabs(wrot.z - r.lastRot.z) + std::fabs(wrot.w - r.lastRot.w) > 1e-4f;
            if (moved) {
                if (d.motion == (int)BodyType::Kinematic)
                    bi.MoveKinematic(r.id, toJR(wpos), toJQ(wrot), dt);
                else
                    bi.SetPositionAndRotation(r.id, toJR(wpos), toJQ(wrot), JPH::EActivation::DontActivate);
                r.lastPos = wpos;
                r.lastRot = wrot;
            }
        } else if (rb) {
            // Gameplay velocity override (e.g. CharacterController): push only if
            // it differs from what Jolt already has, so resting bodies can sleep.
            Vec3 cur = fromJ(bi.GetLinearVelocity(r.id));
            if (length(rb->velocity - cur) > 1e-3f) bi.SetLinearVelocity(r.id, toJ(rb->velocity));
            Vec3 curA = fromJ(bi.GetAngularVelocity(r.id));
            if (length(rb->angularVelocity - curA) > 1e-3f) bi.SetAngularVelocity(r.id, toJ(rb->angularVelocity));
            // lockRotation means "the solver never turns this body — gameplay
            // does": a character rig, a look-at, an AI facing its target. So a
            // rotation written by a behavior this frame is pushed INTO the body
            // (and, below, never read back over). Without this the readback
            // reinstates the body's own rotation every frame and any scripted
            // facing silently does nothing.
            float rotDelta = std::fabs(wrot.x - r.lastRot.x) + std::fabs(wrot.y - r.lastRot.y) +
                             std::fabs(wrot.z - r.lastRot.z) + std::fabs(wrot.w - r.lastRot.w);
            if (d.lockRot && rotDelta > 1e-4f) {
                bi.SetRotation(r.id, toJQ(wrot), JPH::EActivation::DontActivate);
                r.lastRot = wrot;
            }
        }
    }

    // Drop bodies whose entity is gone.
    for (auto it = im->bodies.begin(); it != im->bodies.end();) {
        if (seen.count(it->first) == 0) {
            im->removeRec(it->second);
            im->byIndex.erase(it->second.id.GetIndex());
            it = im->bodies.erase(it);
        } else {
            ++it;
        }
    }

    // 2) Simulate.
    im->contacts.reset();
    int steps = (int)std::ceil(dt / (1.0f / 60.0f));
    im->system.Update(dt, steps < 1 ? 1 : (steps > 8 ? 8 : steps), &im->temp, &im->jobs);

    // 3) Read dynamic results back into entities + components.
    for (auto& kv : im->bodies) {
        Entity* e = kv.first;
        Rec& r = kv.second;
        auto* colc = e->getComponent<ColliderComponent>();
        if (colc) colc->overlapping = im->contacts.overlap(r.id.GetIndex());
        if (!r.dynamic) continue;

        Vec3 wpos = fromJR(bi.GetPosition(r.id));
        Vec4 wrot = fromJQ(bi.GetRotation(r.id));
        r.lastPos = wpos;
        r.lastRot = wrot;

        // Gameplay owns the facing of a lockRotation body (see the push above),
        // so only its position comes back from the solver.
        const bool takeRot = !r.desc.lockRot;
        if (e->parent()) {
            Mat4 pm = e->parent()->worldMatrix();
            e->transform.position = transformPoint(inverse(pm), wpos);
            if (takeRot) e->transform.rotation = quatMul(quatConj(mat4Rotation(pm)), wrot);
        } else {
            e->transform.position = wpos;
            if (takeRot) e->transform.rotation = wrot;
        }
        if (auto* rb = e->getComponent<RigidBodyComponent>()) {
            rb->velocity = fromJ(bi.GetLinearVelocity(r.id));
            rb->angularVelocity = fromJ(bi.GetAngularVelocity(r.id));
            // Grounded: short downward ray from the shape centre, ignoring self.
            // Robust to sleeping (contact callbacks stop firing once a body rests).
            Vec3 o = wpos;
            o.y += r.centerY;
            JPH::RRayCast gray(toJR(o), JPH::Vec3(0.0f, -(r.halfY + 0.15f), 0.0f));
            JPH::RayCastResult gr;
            JPH::IgnoreSingleBodyFilter self(r.id);
            rb->grounded = im->system.GetNarrowPhaseQuery().CastRay(gray, gr, {}, {}, self);
        }
    }
}

RayHit PhysicsWorld::raycast(World& world, const Vec3& origin, const Vec3& dir,
                             float maxDist, const Entity* ignore) const {
    RayHit best;
    if (!impl_) return best;
    Vec3 d = normalize(dir);
    JPH::RRayCast ray(toJR(origin), toJ(d * maxDist));
    JPH::RayCastResult res;
    // Sensors are trigger volumes — pickup radii, area triggers, damage zones.
    // They must not stop a ray: a shot has to pass through the medkit you are
    // standing next to and hit the monster behind it, and a line-of-sight test
    // must not think a trigger is a wall.
    JPH::BodyID skip;
    if (ignore) {
        auto it = impl_->bodies.find(const_cast<Entity*>(ignore));
        if (it != impl_->bodies.end()) skip = it->second.id;
    }
    struct Filter : JPH::BodyFilter {
        JPH::BodyID skip;
        bool ShouldCollide(const JPH::BodyID& id) const override { return id != skip; }
        bool ShouldCollideLocked(const JPH::Body& body) const override {
            return !body.IsSensor() && body.GetID() != skip;
        }
    } bodyFilter;
    bodyFilter.skip = skip;
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(
            ray, res, {}, {}, bodyFilter))
        return best;

    best.hit = true;
    best.distance = res.mFraction * maxDist;
    best.point = origin + d * best.distance;
    auto it = impl_->byIndex.find(res.mBodyID.GetIndex());
    best.entity = it != impl_->byIndex.end() ? it->second : nullptr;

    JPH::BodyLockRead lock(impl_->system.GetBodyLockInterface(), res.mBodyID);
    if (lock.Succeeded())
        best.normal = fromJ(lock.GetBody().GetWorldSpaceSurfaceNormal(res.mSubShapeID2, toJR(best.point)));
    return best;
}

} // namespace ae
