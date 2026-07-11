# Working in this Aether Engine project

This is a game project for **Aether Engine**. Everything here is plain JSON or
C++ — scenes, visual scripts, materials, UI, animation graphs, and data tables
are all hand-editable files, designed so a coding agent can read, modify, and
create game content directly.

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

```
AetherRuntime.exe --project . --frames 300                     # run + read logs
AetherRuntime.exe --project . --frames 60 --screenshot out.bmp # visual check
AetherEditor.exe  --project . --resave roundtrip.json          # scene parse check
```

The exes live in the engine install's `bin` directory (dev checkout:
`<engine>/build/bin/Release/`). Put a `Log` node in your graph and grep stdout
for `[Script] ...` to prove logic ran; watch for `[warn] unknown ...` lines,
which mean a typo'd node/component type. JSON edits need no compile step —
they take effect on the next run.
