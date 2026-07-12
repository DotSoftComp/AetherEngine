# Working in this Aether Engine project

This is a game project for **Aether Engine**. Everything here is plain JSON or
C++ — scenes, visual scripts, materials, UI, animation graphs, and data tables
are all hand-editable files, designed so a coding agent can read, modify, and
create game content directly.

**This is the FPS starter kit** — read [Docs/starter-kit-fps.md](Docs/starter-kit-fps.md) for how the shooting/targets/mission/HUD fit together and safe extension recipes.

**Read `Docs/README.md` first** — it indexes the documentation and explains the
headless verification loop. The two files under `Docs/reference/` are generated
from the engine's live registries and list every component type and every
script-graph node with exact JSON keys, pins, and defaults — trust them over
memory, and regenerate them (`AetherDocGen . --with-module`) after adding
components or nodes.

## Layout

| Path | What it is |
|---|---|
| `project.aeproj` | Project manifest: name, startup scene, C++ module, engine-module toggles |
| `assets/maps/*.json` | Scenes/levels — entities with components ([format](Docs/scenes-and-assets.md)) |
| `assets/scripts/*.json` | Visual scripts / blueprints ([format](Docs/script-graphs.md)) |
| `assets/data/*.json` | Data tables — tuning values read by scripts |
| `assets/{materials,anim,ui,dialogue,missions,entities}/` | Material graphs, anim state machines, UI documents, dialogue, missions, prefabs |
| `Source/` | Native C++ scripts ([SDK guide](Docs/scripting-cpp.md)) — compiled via Tools > Compile Scripts |
| `Plugins/<Name>/` | Drop-in native plugins ([guide](Docs/modules-and-plugins.md)) |
| `Docs/` | Full documentation + generated reference |
| `Intermediate/`, `Binaries/`, `Saves/` | Build products and save games — don't edit |

## Making gameplay changes

1. **Tuning values** — edit `assets/data/*.json` (or the map JSON directly).
2. **Behavior/logic** — author a script graph in `assets/scripts/` and attach
   it with a `ScriptGraph` component; the node reference + authoring guide is
   in `Docs/script-graphs.md`. Prefer this over C++ for gameplay logic.
3. **New component types** — C++ in `Source/` (or a plugin); one class +
   `reflect()` + `AE_REGISTER_COMPONENT` is the entire integration.

## Verify your changes (headless — no human needed)

**One command proves a change:**

```
AetherRuntime.exe --project . --verify            # exit 0 = pass, 1 = fail
```

It runs the scene resave round-trip, plays 300 frames (override with
`--frames N`, pick a map with `--map`), and scans the log. The last stdout
line is machine-readable:

```
VERIFY PASS|FAIL {"ok":..,"map":..,"frames":..,"resave":"ok|mismatch",
                  "scriptLogs":N,"errors":[..],"schemaWarnings":[..]}
```

It fails on any `[error]`, any resave drift, and any schema warning — typo'd
component types (reported with the entity name) and script-graph problems
(reported as `file: node 'id' (Type) pin -> problem`: unknown node types,
links to missing nodes, out-of-range outputs). Fix what the messages name and
re-run. `scriptLogs` counts `[Script]` lines from `Log` nodes — put one in
your graph to prove logic executed.

**Visual regressions** — capture a reference once, then compare against it:

```
AetherRuntime.exe --project . --frames 60 --screenshot ref.bmp   # once: the reference
AetherRuntime.exe --project . --compare ref.bmp [--psnrmin 35]   # every change after
```

`--compare` re-captures (same frame count ⇒ renders are bit-identical, PSNR 99)
and prints `COMPARE PASS|FAIL {json}` with psnr/diffPct; on failure it writes
an amplified `*_diff.bmp` heatmap showing exactly which pixels moved — read it
to see WHAT changed. Combine with `--verify` and a compare failure fails the
whole verification.

Other headless flags when you need them individually:

```
AetherRuntime.exe --project . --frames 300                     # run + read logs
AetherRuntime.exe --project . --frames 60 --screenshot out.bmp # visual check
AetherEditor.exe  --project . --resave roundtrip.json          # scene parse check
```

The exes live in the engine install's `bin` directory (dev checkout:
`<engine>/build/bin/Release/`). JSON edits need no compile step — they take
effect on the next run.

## Live editor control (agent bridge)

When the Aether editor is **open**, it hosts a localhost control server:
`GET http://127.0.0.1:3052/health` probes it, `POST /rpc` with
`{"method":"...","params":{...}}` drives it — spawn/modify entities, set
components, start/stop Play, read logs, capture viewport screenshots, trigger
C++ compiles and navmesh bakes. Access is **PulseLABS-gated**: requests need
the `bridgeToken` from `%APPDATA%/AetherEngine/pulse.json` as an
`x-aether-token` header. The full method reference is
[Docs/reference/agent-bridge.md](Docs/reference/agent-bridge.md) (or call
`{"method":"bridge.help"}`). Prefer the bridge over headless runs when an
editor is already open — you verify against the live session the developer is
looking at.
