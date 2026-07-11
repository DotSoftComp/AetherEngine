# Native C++ scripts and the SDK

When a script graph isn't enough, write a component in C++. Scripts live in
`Source/`, compile into `Binaries/GameModule.dll`, and hot-reload into the
running editor. Plugins (`Plugins/<Name>/Source/`) use the exact same SDK —
see [modules-and-plugins.md](modules-and-plugins.md).

## Anatomy of a script

```cpp
// Source/Bobber.h
#pragma once
#include <ae.h>

class Bobber : public ae::Behavior {
public:
    AE_COMPONENT(Bobber, "Bobber", "Scripts")   // type name, display name, category

    float height = 0.5f;
    float speed = 2.0f;

    void reflect(ae::PropertyVisitor& v) override {
        v.visit("height", height, {ae::PropKind::Default, "Height", 0.02f});
        v.visit("speed",  speed,  {ae::PropKind::Default, "Speed",  0.05f});
    }

    void onStart() override { base_ = entity().transform.position.y; }
    void onUpdate(float dt) override {
        entity().transform.position.y =
            base_ + std::sin(world().time() * speed) * height;
    }

private:
    float base_ = 0.0f;   // not reflected -> not serialized
};
```

```cpp
// Source/Bobber.cpp
#include "Bobber.h"
AE_REGISTER_COMPONENT(Bobber)
```

That is the whole integration. `reflect()` drives serialization, the Details
inspector, prefabs, and undo — there is no other glue. The component appears
under Add Component > Scripts and in scene JSON as
`{ "type": "Bobber", "height": 0.5, "speed": 2 }`.

## What you can reach from a script

- `entity()` — the owning entity: `transform` (position/rotation/scaling),
  `name()`, `getComponent<T>()`, `addComponent<T>()`, `worldMatrix()`,
  `worldPosition()`.
- `world()` — the scene: `spawn()/destroy()/find()/findByGuid()`, `input()`,
  `actions` (input map), `time()/dt()`, `camera()`, `requestCamera()`,
  `missions` (story flags), `physics` (raycasts), `nav` (navmesh queries),
  `requestSaveGame()/requestLoadGame()`.
- Lifecycle: `onStart()` (first tick), `onUpdate(dt)` (every tick during
  Play/runtime), `onDeserialized(assets)` (after load — resolve asset paths
  here).
- Engine components (`ae::LightComponent`, `ae::RigidBodyComponent`,
  `ae::NavAgentComponent`...) are all reachable via `getComponent<T>()` —
  headers under the SDK's `engine/`, `physics/`, `ai/` folders.

## Compiling

- Editor: **Tools > Compile Scripts** (Ctrl+Shift+B). Builds with CMake + the
  same MSVC toolchain as the engine, then hot-swaps the DLL (the scene
  round-trips through a snapshot, so live component state survives as its
  serialized form).
- The build scaffold (`Source/CMakeLists.txt`) is regenerated on every
  compile — don't hand-edit it; just add `.cpp`/`.h` files to `Source/`.
- ABI: the DLL must match the engine's `AE_ABI_VERSION`; a mismatch refuses to
  load with a clear log line — recompile.

## Rules that keep you out of trouble

- Reflected fields are the save format. Renaming a key breaks existing scenes;
  add new keys instead, and keep defaults sensible.
- Don't cache raw `Entity*` across frames unless you own its lifetime —
  prefer guid lookups (`world().findByGuid`).
- Never destroy the world from inside a component tick — request it
  (save/load requests, deferred destroy) like the built-ins do.
- `AE_LOG("...")`/`AE_WARN`/`AE_ERROR` print to stdout — the headless
  verification loop (see [README.md](README.md)) greps these.
