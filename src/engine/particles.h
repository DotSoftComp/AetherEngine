// Aether Engine — Particles component: a CPU-simulated emitter every genre
// needs (sparks, fire, smoke, magic, pickups). Attach to an entity, tune the
// reflected fields in the Details panel, press Play. Emission points along the
// entity's local +Y (rotate the entity to aim); world-space mode leaves a
// trail behind a moving emitter. HDR start/end colors glow through bloom;
// additive vs. alpha blending per emitter. Simulation runs while behaviors
// tick (Play/runtime); rendering is the renderer's particle pass.
#pragma once
#include "component.h"
#include "entity.h"
#include "reflect.h"
#include "../scene/scene.h"
#include <vector>

namespace ae {

class ParticlesComponent : public Component {
public:
    // Emission
    float rate = 40.0f;         // particles per second (0 = off)
    float lifetime = 1.6f;      // seconds (± jitter)
    float lifetimeJitter = 0.3f; // 0..1 fraction
    float speed = 3.0f;         // initial m/s along the cone (± jitter)
    float speedJitter = 0.3f;
    float spreadDeg = 20.0f;    // cone half-angle around local +Y
    bool worldSpace = true;     // simulate in world space (moving emitters trail)
    int maxParticles = 512;

    // Over lifetime
    float gravity = -3.0f;      // world-Y acceleration
    float drag = 0.4f;          // velocity damping per second
    float sizeStart = 0.22f;
    float sizeEnd = 0.05f;
    Vec4 colorStart{4.0f, 2.2f, 0.6f, 1.0f}; // HDR — bright spark orange
    Vec4 colorEnd{0.8f, 0.1f, 0.05f, 0.0f};
    bool additive = true;

    const char* typeName() const override { return "Particles"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("rate", rate, {PropKind::Default, "Rate (per sec)", 0.5f, 0.0f, 5000.0f});
        v.visit("lifetime", lifetime, {PropKind::Default, "Lifetime (s)", 0.02f, 0.05f, 60.0f});
        v.visit("lifetimeJitter", lifetimeJitter, {PropKind::SliderNorm, "Lifetime jitter"});
        v.visit("speed", speed, {PropKind::Default, "Speed", 0.05f, 0.0f, 200.0f});
        v.visit("speedJitter", speedJitter, {PropKind::SliderNorm, "Speed jitter"});
        v.visit("spread", spreadDeg, {PropKind::Angle, "Spread", 0.5f, 0.0f, 180.0f});
        v.visit("worldSpace", worldSpace, {PropKind::Default, "World space"});
        v.visit("maxParticles", maxParticles, {PropKind::Default, "Max particles", 1, 1, 10000});
        v.visit("gravity", gravity, {PropKind::Default, "Gravity", 0.05f});
        v.visit("drag", drag, {PropKind::Default, "Drag", 0.01f, 0.0f, 20.0f});
        v.visit("sizeStart", sizeStart, {PropKind::Default, "Size start", 0.01f, 0.0f, 50.0f});
        v.visit("sizeEnd", sizeEnd, {PropKind::Default, "Size end", 0.01f, 0.0f, 50.0f});
        v.visit("colorStart", colorStart, {PropKind::Color, "Color start (HDR)"});
        v.visit("colorEnd", colorEnd, {PropKind::Color, "Color end (HDR)"});
        v.visit("additive", additive, {PropKind::Default, "Additive"});
    }

    void onUpdate(float dt) override;
    void contribute(RenderScene& out) override;

private:
    struct Particle {
        Vec3 pos;  // world or emitter-local (worldSpace flag)
        Vec3 vel;
        float life = 0.0f;
        float maxLife = 1.0f;
        float speedScale = 1.0f;
    };
    std::vector<Particle> pool_;
    float spawnAcc_ = 0.0f;
    unsigned rng_ = 22699u;
    float frand(); // 0..1
};

} // namespace ae
