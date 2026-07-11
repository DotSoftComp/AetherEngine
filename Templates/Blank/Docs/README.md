# Aether Engine — project documentation

This folder ships with every Aether project so that anyone — including a coding
agent working in this repo — can read, modify, and create game content locally.
Everything in an Aether project is plain JSON or C++: there are no opaque
binary assets to fight.

| Doc | What it covers |
|---|---|
| [scenes-and-assets.md](scenes-and-assets.md) | Project layout, scene/map JSON format, prefabs, data tables, and the other JSON asset types |
| [script-graphs.md](script-graphs.md) | The visual-scripting ("blueprint") system: file schema, authoring by hand or by agent, execution model, verification |
| [scripting-cpp.md](scripting-cpp.md) | Native C++ scripts (Source/) and plugins (Plugins/): the SDK, lifecycle, compiling, hot reload |
| [modules-and-plugins.md](modules-and-plugins.md) | Turning engine modules on/off per project; writing drop-in plugins |
| [ai-assistant.md](ai-assistant.md) | The in-editor AI dev team (PulseLABS): setup, pipeline, review workflow |
| [reference/components.md](reference/components.md) | **Generated** — every component type with its exact JSON fields, defaults, and ranges |
| [reference/script-nodes.md](reference/script-nodes.md) | **Generated** — every script-graph node with its pins, params, and defaults |

The two `reference/` files are generated from the engine's live registries
(`AetherDocGen <projectDir> --with-module`, or Tools > Regenerate Doc Reference
in the editor). Regenerate them after adding components or nodes — never edit
them by hand.

## The verification loop (important for agents)

Aether runs headless, so changes can be verified without a human at the screen:

```
# run the game N frames, then quit (logs go to stdout)
AetherRuntime.exe --project <projectDir> --frames 300

# same, but load a specific map and save a screenshot
AetherRuntime.exe --project <projectDir> --map assets/maps/test.json --frames 60 --screenshot out.bmp

# open the editor headless for a screenshot (edit-mode view)
AetherEditor.exe --project <projectDir> --frames 40 --screenshot out.bmp

# load + resave a scene (round-trip check for hand-edited files)
AetherEditor.exe --project <projectDir> --resave out.json
```

The `Log` script node prints `[Script] <text>` to stdout — put one in a graph
and grep the run output to verify gameplay logic end to end. Malformed JSON or
unknown node/component types produce loud `[warn]`/`[error]` lines instead of
silent failures.
