// Aether Engine — script SDK umbrella header.
//
// A game script derives from ae::Behavior, declares its identity with
// AE_COMPONENT, exposes fields via reflect(), and registers itself with
// AE_REGISTER_COMPONENT in its .cpp:
//
//   // Source/Rotator.h
//   #include <ae.h>
//   class Rotator : public ae::Behavior {
//   public:
//       AE_COMPONENT(Rotator, "Rotator", "Scripts")
//       float speed = 45.0f;
//       void reflect(ae::PropertyVisitor& v) override { v.visit("speed", speed); }
//       void onUpdate(float dt) override {
//           entity().transform.rotateAxis(ae::Vec3(0, 1, 0), ae::radians(speed) * dt);
//       }
//   };
//
//   // Source/Rotator.cpp
//   #include "Rotator.h"
//   AE_REGISTER_COMPONENT(Rotator)
//
// From inside a script: entity() is the owning entity (transform, name,
// getComponent<T>(), addComponent<T>()), world() the scene (spawn/destroy/
// find/findByGuid, input(), time(), dt(), requestCamera, missions).
#pragma once

#include "core/math3d.h"
#include "core/log.h"
#include "core/input.h" // Input (read via world().input()); no windowing in the SDK
#include "engine/component.h"
#include "engine/entity.h"
#include "engine/world.h"
#include "engine/components.h"
#include "engine/behaviors.h"
#include "engine/camera_rigs.h"
#include "engine/assets.h"
#include "engine/mission.h"
#include "engine/reflect.h"
#include "engine/component_registry.h"
