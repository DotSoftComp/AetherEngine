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
| [reference/agent-bridge.md](reference/agent-bridge.md) | **Generated** — the live-editor control API (spawn/modify entities, Play, logs, screenshots) for PulseLABS tooling |

The `reference/` files are generated from the engine's live registries
(`AetherDocGen <projectDir> --with-module`, or Tools > Regenerate Doc Reference
in the editor). Regenerate them after adding components or nodes — never edit
them by hand.

## The verification loop (important for agents)

Aether runs headless, so changes can be verified without a human at the screen.
The one-command form:

```
# resave round-trip + 300 frames + log scan; exits nonzero on failure and
# prints a final machine-readable line: VERIFY PASS|FAIL {json report}
AetherRuntime.exe --project <projectDir> --verify [--map m] [--frames N]
```

`--verify` fails on any `[error]`, scene resave drift, and every schema
warning — unknown component types (with the entity name) and script-graph
link problems (file:node:pin precision). The individual flags:

```
# run the game N frames, then quit (logs go to stdout)
AetherRuntime.exe --project <projectDir> --frames 300

# same, but load a specific map and save a screenshot
AetherRuntime.exe --project <projectDir> --map assets/maps/test.json --frames 60 --screenshot out.bmp

# visual regression: compare a fresh capture against a saved reference
# (COMPARE PASS|FAIL {json}; failures write an amplified *_diff.bmp heatmap)
AetherRuntime.exe --project <projectDir> --compare ref.bmp [--psnrmin 35]

# open the editor headless for a screenshot (edit-mode view)
AetherEditor.exe --project <projectDir> --frames 40 --screenshot out.bmp

# load + resave a scene (round-trip check for hand-edited files)
AetherEditor.exe --project <projectDir> --resave out.json
```

The `Log` script node prints `[Script] <text>` to stdout — put one in a graph
and grep the run output to verify gameplay logic end to end. Malformed JSON or
unknown node/component types produce loud `[warn]`/`[error]` lines instead of
silent failures.

When the **editor is already open**, there is a second path: the agent bridge,
a localhost control server that drives the live editor (spawn/modify entities,
start/stop Play, read logs, screenshot the viewport). It is gated to PulseLABS
tooling by a shared token — see
[reference/agent-bridge.md](reference/agent-bridge.md).
