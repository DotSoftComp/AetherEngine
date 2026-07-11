// Aether Engine — headless physics smoke test.
//
// No window/GL: builds Worlds with colliders, ticks World::update (which steps
// the solver), and asserts real rigid-body behaviour — a sphere settles at the
// right height, and a tilted box tumbles under gravity (angular dynamics) and
// comes to rest. Exit code 0 = pass.
#include "engine/world.h"
#include "physics/physics_components.h"
#include <cmath>
#include <cstdio>

using namespace ae;

static Entity* addGround(World& w) {
    Entity* g = w.spawn("Ground");
    g->transform.scaling = Vec3(20, 1, 20);
    auto* c = g->addComponent<ColliderComponent>();
    c->shape = (int)ColliderShape::Box;
    c->halfExtents = Vec3(0.5f, 0.1f, 0.5f); // world top face at y = 0.1
    return g;
}

// Sphere dropped from height must fall and rest at ground-top + radius.
static bool testSphereRest() {
    World w;
    addGround(w);
    Entity* ball = w.spawn("Ball");
    ball->transform.position = Vec3(0, 6, 0);
    auto* rb = ball->addComponent<RigidBodyComponent>();
    rb->bodyType = (int)BodyType::Dynamic;
    auto* c = ball->addComponent<ColliderComponent>();
    c->shape = (int)ColliderShape::Sphere;
    c->radius = 0.5f;

    Input in;
    const float dt = 1.0f / 60.0f;
    float minY = 6.0f;
    for (int i = 0; i < 240; ++i) {
        w.update(dt, i * dt, in, true);
        minY = std::min(minY, ball->transform.position.y);
    }
    float restY = ball->transform.position.y;
    bool pass = minY < 5.0f && std::fabs(restY - 0.6f) < 0.06f && rb->grounded &&
                std::isfinite(restY);
    std::printf("[PhysSmoke] sphere: min=%.2f rest=%.3f (exp 0.600) grounded=%d -> %s\n",
                minY, restY, rb->grounded, pass ? "PASS" : "FAIL");
    return pass;
}

// Box dropped tilted must acquire angular velocity (tumble) and settle finite.
static bool testBoxTumble() {
    World w;
    addGround(w);
    Entity* box = w.spawn("Box");
    box->transform.position = Vec3(0, 3, 0);
    box->transform.rotation = quatAxisAngle(Vec3(0, 0, 1), radians(35.0f)); // start tilted
    auto* rb = box->addComponent<RigidBodyComponent>();
    rb->bodyType = (int)BodyType::Dynamic;
    rb->friction = 0.6f;
    auto* c = box->addComponent<ColliderComponent>();
    c->shape = (int)ColliderShape::Box;
    c->halfExtents = Vec3(0.5f, 0.5f, 0.5f);

    Input in;
    const float dt = 1.0f / 60.0f;
    float maxAngSpeed = 0.0f, minY = 3.0f;
    for (int i = 0; i < 300; ++i) {
        w.update(dt, i * dt, in, true);
        maxAngSpeed = std::max(maxAngSpeed, length(rb->angularVelocity));
        minY = std::min(minY, box->transform.position.y);
    }
    // The corner strikes first, so the box must rotate (angular velocity appears),
    // then settle: finite pose, low residual spin, resting near the surface.
    float restY = box->transform.position.y;
    float finalSpin = length(rb->angularVelocity);
    bool rotated = maxAngSpeed > 0.4f;
    bool settled = std::isfinite(restY) && restY > 0.2f && restY < 1.2f && finalSpin < 0.5f;
    bool pass = rotated && settled && rb->grounded;
    std::printf("[PhysSmoke] box: maxAngSpeed=%.2f rest=%.3f finalSpin=%.3f grounded=%d -> %s\n",
                maxAngSpeed, restY, finalSpin, rb->grounded, pass ? "PASS" : "FAIL");
    return pass;
}

int main() {
    bool ok = true;
    ok &= testSphereRest();
    ok &= testBoxTumble();
    std::printf("[PhysSmoke] %s\n", ok ? "ALL PASS" : "FAIL");
    return ok ? 0 : 1;
}
