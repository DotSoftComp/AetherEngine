// Aether Engine — example gameplay behaviors, showing how scripts drive entities
// through the same component lifecycle the game logic will use.
#pragma once
#include "entity.h"
#include "world.h"
#include "components.h"
#include "../core/window.h"

namespace ae {

// Free-fly camera controller: WASD/QE move, right-mouse look, shift to sprint.
// Owns yaw/pitch itself and writes them straight into the entity's transform
// rotation each frame, so a CameraComponent on the same entity (or anything
// else reading the transform) sees a consistent orientation.
class CameraController : public Behavior {
public:
    float moveSpeed = 4.5f;
    float sprintSpeed = 14.0f;
    float lookSens = 0.18f;
    float yaw = -90.0f;   // degrees; 0 looks down -Z, -90 looks down +X
    float pitch = -10.0f;

    const char* typeName() const override { return "CameraController"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("moveSpeed", moveSpeed, {PropKind::Default, "Move speed"});
        v.visit("sprintSpeed", sprintSpeed, {PropKind::Default, "Sprint speed"});
        v.visit("lookSens", lookSens, {PropKind::Default, "Look sens", 0.005f});
        v.visit("yaw", yaw, {PropKind::Angle, "Yaw", 0.5f});
        v.visit("pitch", pitch, {PropKind::Angle, "Pitch", 0.5f});
    }

    void onUpdate(float dt) override {
        const Input& in = world().input();

        if (in.mouseButtons[1]) {
            yaw += in.mouseDX * lookSens;
            pitch = clampf(pitch - in.mouseDY * lookSens, -89.0f, 89.0f);
        }
        entity().transform.rotation = quatMul(quatAxisAngle(Vec3(0, 1, 0), radians(yaw)),
                                              quatAxisAngle(Vec3(1, 0, 0), radians(pitch)));

        float speed = (in.keys[0x10] ? sprintSpeed : moveSpeed) * dt; // 0x10 = VK_SHIFT
        Vec3 fwd = quatRotate(entity().transform.rotation, Vec3(0, 0, -1));
        Vec3 right = normalize(cross(fwd, Vec3(0, 1, 0)));
        Vec3& p = entity().transform.position;
        if (in.keys['W']) p += fwd * speed;
        if (in.keys['S']) p += fwd * -speed;
        if (in.keys['D']) p += right * speed;
        if (in.keys['A']) p += right * -speed;
        if (in.keys['E']) p += Vec3(0, 1, 0) * speed;
        if (in.keys['Q']) p += Vec3(0, -1, 0) * speed;
    }
};

// Moves the entity on a horizontal circle with a little vertical bob — used for
// the orbiting demo lights.
class OrbitBehavior : public Behavior {
public:
    float radius = 5.5f;
    float speed = 0.5f;
    float phase = 0.0f;
    float bobAmp = 0.6f;
    float bobSpeed = 1.7f;
    float height = 1.4f;

    const char* typeName() const override { return "Orbit"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("radius", radius, {PropKind::Default, "Radius"});
        v.visit("speed", speed, {PropKind::Default, "Speed", 0.01f});
        v.visit("phase", phase, {PropKind::Default, "Phase", 0.01f});
        v.visit("bobAmp", bobAmp, {PropKind::Default, "Bob amp", 0.02f});
        v.visit("bobSpeed", bobSpeed, {PropKind::Default, "Bob speed", 0.02f});
        v.visit("height", height, {PropKind::Default, "Height"});
    }

    void onUpdate(float) override {
        float t = world().time();
        float a = t * speed + phase;
        entity().transform.position =
            Vec3(std::cos(a) * radius, height + std::sin(t * bobSpeed + phase) * bobAmp,
                 std::sin(a) * radius);
    }
};

// Spins the entity about an axis at a constant rate.
class SpinBehavior : public Behavior {
public:
    Vec3 axis{0, 1, 0};
    float degreesPerSec = 45.0f;

    const char* typeName() const override { return "Spin"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("axis", axis, {PropKind::Default, "Axis", 0.02f, -1.0f, 1.0f});
        v.visit("degreesPerSec", degreesPerSec, {PropKind::Default, "Deg/sec", 0.5f});
    }

    void onUpdate(float dt) override {
        entity().transform.rotateAxis(axis, radians(degreesPerSec) * dt);
    }
};

} // namespace ae
