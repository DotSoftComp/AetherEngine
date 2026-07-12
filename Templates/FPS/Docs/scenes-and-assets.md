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
| Anim graph | `assets/anim/fox.json` | states (clip/speed/loop) + parameter-conditioned transitions with blend times; parameters set from scripts via `SetAnimParam` |
| UI document | `assets/ui/hud.json` | widget tree (Panel/Label/Button/ProgressBar) with anchor+pivot layout; `{flag:name}` text binding; Buttons fire the `OnUIButton` script event |
| Material graph | `assets/materials/energy.json` | dataflow nodes -> PBR output; compiled to GLSL at load; Ctrl+S in the canvas hot-reloads |
| Input map | `assets/input.json` | named actions (buttons) + axes bound to keys/mouse/gamepad; read via `OnAction`/`IsActionDown`/`GetAxis` and by CharacterController |
| Missions | `assets/missions/missions.json` | missions/objectives + story flags; scripts read/write flags with `GetFlag`/`SetFlag` |

## Verification

After editing any asset by hand, run the headless loop from
[README.md](README.md) — a scene that fails to parse or references missing
assets reports `[error]`/`[warn]` lines naming the file. `--resave` round-trips
a scene through the serializer, normalizing your hand-written file to the
canonical field order.
