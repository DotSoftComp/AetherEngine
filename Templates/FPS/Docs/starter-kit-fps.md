# FPS starter kit — how this project works

A minimal but complete first-person shooter: WASD + mouse look, hitscan
shooting with ammo and reload, six destructible targets, a mission that
completes when the arena is clear, and a HUD bound to live game flags.
Everything is data — no C++ was needed. Use it as a base, and as worked
examples of the engine's idioms.

## The moving parts

| File | What it does |
|---|---|
| `assets/maps/arena.json` | The level: floor/walls/crates (static colliders), 6 targets, Player, FPCamera, GameManager |
| `assets/scripts/player_shoot.json` | On `Fire`: ammo gate → `Raycast` from the camera → publish the hit through flags |
| `assets/scripts/target.json` | One shared graph on every target: react to a new shot, range-test self, die + count |
| `assets/scripts/game_flow.json` | Intro log + win message via `OnFlag kills == 6` |
| `assets/ui/hud.json` | Crosshair, `TARGETS {flag:kills} / 6`, ammo label + bar (`bindFlag: ammo`) |
| `assets/missions/missions.json` | "Clear the Arena": one flag objective — `kills == 6` |
| `assets/data/weapons.json` | Tuning: magazine size, hitscan range, hit radius (hot-reloads during Play) |
| `assets/input.json` | `Fire` (LMB / RB), `Reload` (R / X), `Jump`, plus move/look axes |

## The player rig (two entities, on purpose)

- **Player** — capsule `Collider` + dynamic `RigidBody` (`lockRotation`) +
  `CharacterController` with `faceMoveDir: false`. The controller moves
  relative to the **active camera**, so look direction steers movement.
- **FPCamera** — `Camera` (`isDefault`) + `FirstPersonCamera` targeting the
  Player (rides it at eye height, owns mouse look) + the shooting graph.
  Shooting raycasts from `GetPosition(Self)` along `GetForward(Self)` — the
  crosshair is always truthful.

## Pattern: cross-graph signalling through flags

Script graphs can't call each other; **story flags are the global channel**
(they also drive missions and HUD bindings). A shot publishes four flags:
`hitX/hitY/hitZ` (the impact point) and `shotSeq` (increments per hit).
Every target polls `shotSeq`, and on change measures
`Distance(self, hitPoint) <= hit_radius` → destroys itself and increments
`kills`. The mission and HUD react to `kills` for free.

**Gotcha: flags are integers.** The impact point is stored ×10
(0.1 m precision) and divided back in `target.json`. Pass fractional data
through flags the same way.

## Extension recipes (each is a small, safe diff)

- **More targets** — copy a `Target*` entity in `arena.json` (fresh `guid`!),
  bump the mission objective `value` and the HUD `/ 6` texts.
- **Tune the gun** — edit `assets/data/weapons.json`; it hot-reloads live
  during Play in the editor.
- **Moving targets** — swap the `Spin` component for an `Orbit`, or attach a
  script graph that drives `SetPosition` (see the Sample template's orb).
- **Scoring** — add a `score` flag in `target.json` (e.g. +100 per kill) and
  a HUD label `{flag:score}`.
- **Enemies that chase** — re-enable the `ai` module in `project.aeproj`,
  give a target a `NavAgent` + `AIMoveTo` graph toward the Player.
- **Sounds** — drop a .wav in `assets/audio/`, add an `AudioSource`, trigger
  with the `PlaySound` node on fire/hit.

Note this project **disables** the `narrative`, `ai`, and `animation` engine
modules in `project.aeproj` — it doesn't use them. Re-enable per line when an
extension needs one.

## Verify after changing anything

```
AetherRuntime.exe --project . --verify
```

Expect `[FPS] shooter ready` and `[FPS] Clear the Arena` in `scriptLogs`, and
`VERIFY PASS`. Gameplay can be exercised headlessly too: publish the same
flags a shot would (see the pattern above) from a temporary test graph and
grep for `[FPS] target down!`.
