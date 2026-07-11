// OrbitFx plugin sample: pulses the Light on this entity with a sine wave.
// Demonstrates that a drop-in plugin gets the exact same SDK as game scripts —
// components, reflection, the inspector, serialization, hot reload.
#pragma once
#include <ae.h>

class PulseLight : public ae::Behavior {
public:
    AE_COMPONENT(PulseLight, "Pulse Light", "OrbitFx")

    float speed = 2.0f;  // pulses per second (radians/s factor)
    float amount = 0.6f; // 0 = steady, 1 = fully off at the trough

    void reflect(ae::PropertyVisitor& v) override {
        v.visit("speed", speed, {ae::PropKind::Default, "Pulse speed", 0.05f, 0.0f, 20.0f});
        v.visit("amount", amount, {ae::PropKind::SliderNorm, "Pulse amount"});
    }

    void onStart() override {
        if (auto* l = entity().getComponent<ae::LightComponent>()) {
            base_ = l->intensity;
            AE_LOG("[OrbitFx] PulseLight active on '%s'", entity().name().c_str());
        }
    }
    void onUpdate(float) override {
        auto* l = entity().getComponent<ae::LightComponent>();
        if (!l) return;
        float s = 0.5f + 0.5f * std::sin(world().time() * speed * 6.2831853f);
        l->intensity = base_ * (1.0f - amount * s);
    }

private:
    float base_ = 1.0f;
};
