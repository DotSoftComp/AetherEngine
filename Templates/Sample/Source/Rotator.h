// Sample Aether script: spins its entity around an axis every frame.
// Attach it from Details > Add Component > Scripts > Rotator.
#pragma once
#include <ae.h>

class Rotator : public ae::Behavior {
public:
    AE_COMPONENT(Rotator, "Rotator", "Scripts")

    float speed = 45.0f;              // degrees per second
    ae::Vec3 axis{0.0f, 1.0f, 0.0f};

    void reflect(ae::PropertyVisitor& v) override {
        v.visit("speed", speed, {ae::PropKind::Default, "Speed (deg/s)", 0.5f});
        v.visit("axis", axis, {ae::PropKind::Default, "Axis", 0.02f, -1.0f, 1.0f});
    }

    void onUpdate(float dt) override {
        entity().transform.rotateAxis(axis, ae::radians(speed) * dt);
    }
};
