# Component reference

GENERATED from the live component registry (AetherDocGen) - do not edit.
Every component serializes into a scene entity's `components` array as
`{ "type": "<Type>", <field>: <value>, ... }`. Field keys below are the
exact JSON keys; omitted fields keep their defaults. Ranges are editor
hints, not validation.

## MeshRenderer

- display name: Mesh Renderer  
- category: Rendering  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `mesh` | string | `""` | Mesh |  |
| `texSet` | string | `""` | Textures |  |
| `castShadow` | bool | `true` | Cast shadow |  |
| `drawDistance` | float | `0` | Draw distance (0 = inf) | 0 .. 10000 |
| `material.baseColor` | vec4 | `[1, 1, 1, 1]` | Base Color |  |
| `material.metallic` | float | `0` | Metallic |  |
| `material.roughness` | float | `0.5` | Roughness |  |
| `material.emissive` | vec3 | `[0, 0, 0]` | Emissive |  |
| `material.uvScale` | float | `1` | UV Scale | 0.05 .. 32 |
| `material.normalScale` | float | `1` | Normal Scale |  |
| `material.occlusionStrength` | float | `1` | Occlusion |  |
| `material.alphaCutoff` | float | `-1` | Alpha Cutoff |  |
| `material.doubleSided` | bool | `false` | Double-sided |  |
| `material.blend` | bool | `false` | Transparent (blend) |  |
| `material.graph` | string | `""` | Material Graph |  |
| `material.subsurface` | float | `0` | Subsurface |  |
| `material.sssCurvature` | float | `0` | SSS Curvature |  |
| `material.translucency` | float | `0` | Translucency |  |
| `material.sssTint` | vec3 | `[1, 0.3, 0.2]` | SSS Tint |  |

## Light

- display name: Light  
- category: Rendering  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `lightType` | int | `0` | Type (0=Point 1=Spot 2=Dir) | 0 .. 2 |
| `color` | vec3 | `[1, 1, 1]` | Color (radiance) | 0 .. 200 |
| `intensity` | float | `1` | Intensity | 0 .. 100 |
| `range` | float | `12` | Range | 0 .. 500 |
| `innerAngle` | float | `25` | Spot inner | 0 .. 89 |
| `outerAngle` | float | `35` | Spot outer | 0 .. 89 |

## Camera

- display name: Camera  
- category: Rendering  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `fov` | float | `55` | FOV | 10 .. 120 |
| `near` | float | `0.1` | Near | 0.01 .. 10 |
| `far` | float | `300` | Far | 10 .. 5000 |
| `isDefault` | bool | `false` | Default gameplay camera |  |

## Model

- display name: Model  
- category: Rendering  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `path` | string | `""` | Model |  |
| `animate` | bool | `true` | Animate |  |

## Spin

- display name: Spin  
- category: Behaviors  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `axis` | vec3 | `[0, 1, 0]` | Axis | -1 .. 1 |
| `degreesPerSec` | float | `45` | Deg/sec |  |

## Orbit

- display name: Orbit  
- category: Behaviors  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `radius` | float | `5.5` | Radius |  |
| `speed` | float | `0.5` | Speed |  |
| `phase` | float | `0` | Phase |  |
| `bobAmp` | float | `0.6` | Bob amp |  |
| `bobSpeed` | float | `1.7` | Bob speed |  |
| `height` | float | `1.4` | Height |  |

## CameraController

- display name: Camera Controller  
- category: Behaviors  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `moveSpeed` | float | `4.5` | Move speed |  |
| `sprintSpeed` | float | `14` | Sprint speed |  |
| `lookSens` | float | `0.18` | Look sens |  |
| `yaw` | float | `-90` | Yaw |  |
| `pitch` | float | `-10` | Pitch |  |

## ThirdPersonCamera

- display name: Third-Person Camera  
- category: Behaviors  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `targetGuid` | entity guid | `""` | Target |  |
| `targetName` | entity name (fallback) | `""` | Target |  |
| `distance` | float | `5` | Distance | 0.5 .. 50 |
| `lookHeight` | float | `1.2` | Look height |  |
| `yaw` | float | `0` | Yaw |  |
| `pitch` | float | `-12` | Pitch |  |

## FollowCamera

- display name: Follow Camera  
- category: Behaviors  
- source: core

| field | type | default | label | range |
|---|---|---|---|---|
| `targetGuid` | entity guid | `""` | Target |  |
| `targetName` | entity name (fallback) | `""` | Target |  |
| `offset` | vec3 | `[0, 3, 6]` | Offset |  |
| `lookHeight` | float | `1.2` | Look height |  |
| `smoothing` | float | `5` | Smoothing | 0.1 .. 30 |

## RigidBody

- display name: Rigid Body  
- category: Physics  
- source: module: physics

| field | type | default | label | range |
|---|---|---|---|---|
| `bodyType` | int | `1` | Type (0=Static 1=Dyn 2=Kin) | 0 .. 2 |
| `mass` | float | `1` | Mass | 0 .. 1000 |
| `useGravity` | bool | `true` | Use gravity |  |
| `linearDamping` | float | `0.05` | Linear damping | 0 .. 1 |
| `angularDamping` | float | `0.05` | Angular damping | 0 .. 1 |
| `restitution` | float | `0` | Restitution |  |
| `friction` | float | `0.5` | Friction |  |
| `lockRotation` | bool | `false` | Lock rotation |  |

## Collider

- display name: Collider  
- category: Physics  
- source: module: physics

| field | type | default | label | range |
|---|---|---|---|---|
| `shape` | int | `0` | Shape (0=Box 1=Sphere 2=Capsule) | 0 .. 2 |
| `halfExtents` | vec3 | `[0.5, 0.5, 0.5]` | Half extents | 0 .. 100 |
| `radius` | float | `0.5` | Radius | 0 .. 100 |
| `height` | float | `2` | Height | 0 .. 100 |
| `center` | vec3 | `[0, 0, 0]` | Center offset |  |
| `isTrigger` | bool | `false` | Is trigger |  |

## CharacterController

- display name: Character Controller  
- category: Physics  
- source: module: physics

| field | type | default | label | range |
|---|---|---|---|---|
| `moveSpeed` | float | `4.5` | Move speed | 0 .. 50 |
| `sprintSpeed` | float | `8` | Sprint speed | 0 .. 50 |
| `jumpSpeed` | float | `5` | Jump speed | 0 .. 50 |
| `faceMoveDir` | bool | `true` | Face move direction |  |

## AudioSource

- display name: Audio Source  
- category: Audio  
- source: module: audio

| field | type | default | label | range |
|---|---|---|---|---|
| `clip` | string | `""` | Clip |  |
| `volume` | float | `1` | Volume | 0 .. 1 |
| `loop` | bool | `false` | Loop |  |
| `spatial` | bool | `true` | 3D / spatial |  |
| `playOnStart` | bool | `true` | Play on start |  |
| `minDistance` | float | `5` | Min distance | 0 .. 500 |
| `maxDistance` | float | `50` | Max distance | 0 .. 2000 |

## ScriptGraph

- display name: Script Graph  
- category: Scripting  
- source: module: scripting

| field | type | default | label | range |
|---|---|---|---|---|
| `graph` | string | `""` | Graph file |  |

## UIDocument

- display name: UI Document  
- category: UI  
- source: module: ui

| field | type | default | label | range |
|---|---|---|---|---|
| `doc` | string | `""` | UI document |  |

## Particles

- display name: Particles  
- category: Effects  
- source: module: particles

| field | type | default | label | range |
|---|---|---|---|---|
| `rate` | float | `40` | Rate (per sec) | 0 .. 5000 |
| `lifetime` | float | `1.6` | Lifetime (s) | 0.05 .. 60 |
| `lifetimeJitter` | float | `0.3` | Lifetime jitter |  |
| `speed` | float | `3` | Speed | 0 .. 200 |
| `speedJitter` | float | `0.3` | Speed jitter |  |
| `spread` | float | `20` | Spread | 0 .. 180 |
| `worldSpace` | bool | `true` | World space |  |
| `maxParticles` | int | `512` | Max particles | 1 .. 10000 |
| `gravity` | float | `-3` | Gravity |  |
| `drag` | float | `0.4` | Drag | 0 .. 20 |
| `sizeStart` | float | `0.22` | Size start | 0 .. 50 |
| `sizeEnd` | float | `0.05` | Size end | 0 .. 50 |
| `colorStart` | vec4 | `[4, 2.2, 0.6, 1]` | Color start (HDR) |  |
| `colorEnd` | vec4 | `[0.8, 0.1, 0.05, 0]` | Color end (HDR) |  |
| `additive` | bool | `true` | Additive |  |

## Animator

- display name: Animator  
- category: Animation  
- source: module: animation

| field | type | default | label | range |
|---|---|---|---|---|
| `graph` | string | `""` | Anim graph |  |

## NavAgent

- display name: Nav Agent  
- category: AI  
- source: module: ai

| field | type | default | label | range |
|---|---|---|---|---|
| `speed` | float | `3` | Speed | 0 .. 50 |
| `acceptance` | float | `0.35` | Acceptance radius | 0.05 .. 5 |
| `repathInterval` | float | `0.5` | Repath interval (s) | 0 .. 10 |
| `faceMoveDir` | bool | `true` | Face move direction |  |

## DialogueTrigger

- display name: Dialogue Trigger  
- category: Narrative  
- source: module: narrative

| field | type | default | label | range |
|---|---|---|---|---|
| `scene` | string | `""` | Scene file |  |
| `radius` | float | `3` | Radius | 0.1 .. 100 |
| `once` | bool | `false` | Once |  |
| `player` | string | `"MainCamera"` | Player entity |  |

