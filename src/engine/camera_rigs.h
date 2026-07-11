// Aether Engine — gameplay camera rigs. Each is a Behavior that positions and
// orients its own entity every frame; pair it with a CameraComponent on the
// same entity to turn that into a usable camera (see World's camera director
// in world.h for how multiple cameras coexist and get selected/blended).
//
// A camera with neither of these attached — just a CameraComponent on a
// statically-placed entity — is a "fixed" camera: no code needed at all.
#pragma once
#include "entity.h"
#include "world.h"
#include "reflect.h"
#include "../core/window.h"
#include <cmath>
#include <string>

namespace ae {

// Orbiting third-person camera: stays behind/above a target entity at a fixed
// distance, orbit controlled by right-mouse drag (same convention as
// CameraController). This is the classic over-the-shoulder gameplay camera.
class ThirdPersonCameraBehavior : public Behavior {
public:
    // Durable reference to the followed entity (resolved through the World by
    // Guid). `targetName` is a legacy/authoring fallback used only when the
    // ref is empty — the editor's picker fills `targetRef`.
    EntityRef targetRef;
    std::string targetName;
    float distance = 5.0f;
    float lookHeight = 1.2f; // point above the target's position to orbit around/aim at
    float yaw = 0.0f, pitch = -12.0f;
    float lookSens = 0.18f;
    float minPitch = -80.0f, maxPitch = 80.0f;

    const char* typeName() const override { return "ThirdPersonCamera"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("targetGuid", "targetName", targetRef, targetName, {PropKind::Default, "Target"});
        v.visit("distance", distance, {PropKind::Default, "Distance", 0.05f, 0.5f, 50.0f});
        v.visit("lookHeight", lookHeight, {PropKind::Default, "Look height", 0.02f});
        v.visit("yaw", yaw, {PropKind::Angle, "Yaw", 0.5f});
        v.visit("pitch", pitch, {PropKind::Angle, "Pitch", 0.5f});
    }

    Entity* resolveTarget() {
        Entity* t = targetRef.get(world());
        if (!t && !targetName.empty()) {
            t = world().find(targetName);
            if (t) targetRef.set(t); // upgrade a name reference to a durable guid
        }
        return t;
    }

    void onUpdate(float) override {
        Entity* target = resolveTarget();
        if (!target) return;
        const Input& in = world().input();
        if (in.mouseButtons[1]) {
            yaw += in.mouseDX * lookSens;
            pitch = clampf(pitch - in.mouseDY * lookSens, minPitch, maxPitch);
        }

        Vec3 focus = target->worldPosition() + Vec3(0, lookHeight, 0);
        float yr = radians(yaw), pr = radians(pitch);
        Vec3 dir(std::cos(pr) * std::cos(yr), std::sin(pr), std::cos(pr) * std::sin(yr));

        entity().transform.position = focus - dir * distance;
        entity().transform.rotation = quatLookAt(dir);
    }
};

// Smoothly trails a target at a world-space offset and looks at it — a
// traveling/dolly camera whose position depends on where the target has
// been, not a rigid parent-child attachment (so it lags behind naturally).
class FollowCameraBehavior : public Behavior {
public:
    EntityRef targetRef;
    std::string targetName; // legacy/authoring fallback (see ThirdPersonCameraBehavior)
    Vec3 offset{0.0f, 3.0f, 6.0f}; // world-space offset added to the target's position
    float lookHeight = 1.2f;
    float smoothing = 5.0f; // higher = snappier, lower = laggier

    const char* typeName() const override { return "FollowCamera"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("targetGuid", "targetName", targetRef, targetName, {PropKind::Default, "Target"});
        v.visit("offset", offset, {PropKind::Default, "Offset"});
        v.visit("lookHeight", lookHeight, {PropKind::Default, "Look height", 0.02f});
        v.visit("smoothing", smoothing, {PropKind::Default, "Smoothing", 0.05f, 0.1f, 30.0f});
    }

    Entity* resolveTarget() {
        Entity* t = targetRef.get(world());
        if (!t && !targetName.empty()) {
            t = world().find(targetName);
            if (t) targetRef.set(t);
        }
        return t;
    }

    void onUpdate(float dt) override {
        Entity* target = resolveTarget();
        if (!target) return;
        Vec3 desired = target->worldPosition() + offset;
        float a = 1.0f - std::exp(-smoothing * dt); // frame-rate-independent lerp factor
        Vec3& p = entity().transform.position;
        p = p + (desired - p) * a;

        Vec3 focus = target->worldPosition() + Vec3(0, lookHeight, 0);
        entity().transform.rotation = quatLookAt(focus - p);
    }
};

} // namespace ae
