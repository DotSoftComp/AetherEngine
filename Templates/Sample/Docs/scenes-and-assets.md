# Scenes, maps, and asset formats

Every asset in an Aether project is JSON (or a standard model/image/audio
format). Paths inside assets are always **project-relative** with forward
slashes (`assets/maps/main.json`), so the project folder is fully relocatable.

## Project layout

```
project.aeproj          project manifest (name, startup scene, module, engine-module flags)
AGENTS.md / CLAUDE.md   agent orientation (you are probably reading it already)
Docs/                   this documentation (+ generated reference/)
assets/
  maps/*.json           scenes ("maps") — the level files
  scripts/*.json        script graphs (visual scripting)
  materials/*.json      material graphs (node-authored shaders)
  anim/*.json           animation state machines (AnimGraph)
  ui/*.json             retained-UI documents (HUD, menus)
  data/*.json           data tables (designer tuning values)
  dialogue/*.json       dialogue scenes
  missions/missions.json  mission/objective definitions
  entities/*.json       prefabs (reusable entity subtrees)
  input.json            action/axis bindings (keyboard/mouse/gamepad)
  *.glb *.gltf *.obj *.fbx  models   |   *.wav  audio   |  images: png/jpg/bmp
Source/                 native C++ scripts -> Binaries/GameModule.dll
Plugins/<Name>/         drop-in native plugins (see modules-and-plugins.md)
Saves/                  runtime save-game slots (written by the SaveGame node)
Intermediate/, Binaries/  build products — never edit, safe to delete
```

## Scene (map) format — `assets/maps/*.json`

```json
{
  "version": 1,
  "environment": { "sunDir": [0.35, 0.55, 0.25], "skyIntensity": 22 },
  "entities": [
    {
      "name": "Crate",
      "guid": "b1e8a1ad99564876a9062828df81bb0f",
      "parent": -1,
      "active": true,
      "position": [0, 0.5, 0],
      "rotation": [0, 0, 0, 1],
      "scale": [1, 1, 1],
      "components": [
        { "type": "MeshRenderer", "mesh": "cube", "texSet": "rust",
          "castShadow": true, "material": { "roughness": 0.6 } },
        { "type": "RigidBody", "bodyType": 1, "mass": 2 },
        { "type": "Collider", "shape": 0, "halfExtents": [0.5, 0.5, 0.5] }
      ]
    }
  ]
}
```

- `guid` — 32 hex chars, unique per entity. Durable references (camera
  targets, script Entity pins) point at guids, so keep them stable; when
  creating entities by hand, any unique 32-hex string works.
- `parent` — index into this same `entities` array (`-1` = root). Children
  inherit the parent transform.
- `rotation` — quaternion `[x, y, z, w]` (`[0,0,0,1]` = identity).
- `components` — see [reference/components.md](reference/components.md) for
  every type and field. Omitted fields keep their defaults. Unknown types are
  skipped with a `[warn]` (this is how disabled engine modules degrade).
- Built-in procedural meshes: `sphere`, `cube`, `plane`, `torus`; built-in
  texture sets: `tile`, `rust`. Model files go through a `Model` component
  (`"path": "assets/Fox.glb"`) instead of MeshRenderer.

Prefabs (`assets/entities/*.json`) use the same entity format and instantiate
with fresh guids.

## Data tables — `assets/data/*.json`

Designer-tunable values, read by scripts via the `GetData` node family:

```json
{
  "columns": ["value", "label"],
  "rows": {
    "boost_impulse": { "value": 7,   "label": "BOOST button strength" },
    "orb_speed":     { "value": 1.2, "label": "orbit speed (rad/s)" }
  }
}
```

Cells are number, bool, or string. Edit by hand or in the editor's Data Table
panel (Ctrl+S hot-reloads live GetData nodes, including during Play).

## Other JSON assets

Each of these has a small, self-describing format — open the example file
listed and copy its shape (all are covered by editor panels too):

| Asset | Example in this project | Notes |
|---|---|---|
| Anim graph | `assets/anim/fox.json` | layered state machines (`animGraph: 2`): each layer has states + parameter-conditioned transitions with blend times; a state is one clip **or a 1D/2D blend space** (`type: blend1d/blend2d`, `paramX`/`paramY`, `motions: [{clip,x,y,speed}]` weighted live, phase-synced). Overlay layers blend on top with `weight` (or a `weightParam`) and an optional `maskBone` restricting them to that bone's subtree (upper-body aiming). The Animator component can also extract **root motion** (`rootMotion: true`, optional `rootBone`) — the root bone's XZ translation moves the entity instead of the mesh. Pair with a `TwoBoneIK` component (end bone + target entity) for feet/hand placement, applied after animation each frame. Every Model instance owns its pose, so many entities can share one glTF and animate independently; glTF **morph targets** (incl. `weights` animations) play automatically. Parameters set from scripts via `SetAnimParam` |
| UI document | `assets/ui/hud.json` | widget tree (Panel/Label/Button/ProgressBar/Image) with anchor+pivot layout; per-widget `fontScale` for text; `image` sprite path (project-relative) for Image widgets; `{flag:name}` text binding; Buttons fire the `OnUIButton` script event and support keyboard/gamepad focus navigation (Down/Tab/DpadDown next, Up/DpadUp prev, Enter/Space/A activate) for real menus |
| Material graph | `assets/materials/energy.json` | dataflow nodes -> PBR output (BaseColor/Metallic/Roughness/Emissive/Opacity/AO + tangent-space **Normal** — feed it from a `NormalMap` node for bumped materials); up to 8 textures per graph; **subgraphs**: a `Subgraph` node inlines another materials/*.json as a function (its `SubInput` nodes = the caller's A-D pins, one `SubOutput` = the result); compiled to GLSL at load; Ctrl+S in the canvas hot-reloads |
| Input map | `assets/input.json` | named actions (buttons) + axes bound to keys/mouse/gamepad; read via `OnAction`/`IsActionDown`/`GetAxis` and by CharacterController |
| Missions | `assets/missions/missions.json` | missions/objectives + story flags; scripts read/write flags with `GetFlag`/`SetFlag` |
| Texture import settings | `<image>.png.import.json` (next to any source image) | overrides how that image imports — `srgb` (false for data maps), `normalMap` (encode two-channel **BC5** + reconstruct Z in the shader — use it for every normal map), `compress` (false = plain RGBA8), `maxSize` (downscale so the longest edge fits), `mipBias` (drop the N largest mips = less VRAM). Missing file = engine defaults (glTF already tags its own colour/normal slots). Editing the image **or** this sidecar hot re-imports it live in the editor — the texture keeps its id, so materials update in place |
| Behavior tree | `assets/ai/*.json` (attach a `BehaviorTree` component) | nested NPC decision tree (`behaviorTree: 1`, `root` node). Composites: `Sequence` (all children succeed), `Selector` (first success), `Parallel`. Decorators: `Inverter`, `Repeat`, `Succeeder` (one `child`). Leaves act/test on the entity: `MoveToTarget`/`MoveToPoint` (drive the sibling NavAgent; `v` = point), `Wait` (`time` s), `Stop`, `Log` (`s` = message), and conditions `CanSeeTarget`/`HasTarget`/`IsAtTarget`. A small blackboard (target entity + position) is written by `CanSeeTarget` (reads the sibling `Perception` component's sight cone + hearing) and read by `MoveToTarget` — so a guard is `Selector[ Sequence[CanSeeTarget, MoveToTarget], Wait ]` |

## Lighting and shadows

The sun (`environment.sunDir`) always casts stabilized cascaded shadows over the
scene. Local `Light` components cast real-time shadows automatically — no
per-light setup: the nearest **spot** lights get a perspective shadow map and the
nearest **point** lights an omnidirectional cube shadow. A mesh casts into them
when its `MeshRenderer.castShadow` is true (the same flag the sun uses).
Shadow-caster counts are capped and reduced on weak GPUs; override with the
`spotShadows` / `pointShadows` game settings (or the `--spotshadows N` /
`--pointshadows N` runtime flags, `0` = off). Directional local lights and the
sun are shadowed by the cascades; only point/spot use the local maps.

## Anti-aliasing and exposure

**TAA** (temporal anti-aliasing) is **on** by default: the projection is jittered
sub-pixel each frame and the previous frame is reprojected and blended in, so
edges and specular shimmer resolve far better than FXAA alone (which still runs
after it). It is deterministic — a fixed-frame capture is still bit-identical,
so `--compare` works unchanged. Toggle with the `taa` game setting or
`--taa 0|1`.

**Auto-exposure** is **off** by default (it re-keys the whole image, so enabling
it changes an existing project's look). On, the renderer measures the frame's
log-average luminance and adapts exposure toward middle grey — a scene stays
readable walking from sunlight into a cave with no per-level tuning — and the
`exposure` setting becomes a relative bias rather than an absolute multiplier.
Enable with the `autoExposure` game setting or `--autoexposure 1`.

## Verification

After editing any asset by hand, run the headless loop from
[README.md](README.md) — a scene that fails to parse or references missing
assets reports `[error]`/`[warn]` lines naming the file. `--resave` round-trips
a scene through the serializer, normalizing your hand-written file to the
canonical field order.
