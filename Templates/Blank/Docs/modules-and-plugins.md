# Engine modules and project plugins

Aether is modular: features you don't use can be switched off per project, and
new native features can be dropped in as plugins — no engine changes.

## Engine modules (turn features off)

The engine's optional features are named modules: `physics`, `audio`,
`scripting`, `ui`, `particles`, `animation`, `ai`, `narrative`. Disable any of
them in `project.aeproj`:

```json
{
  "name": "MyGame",
  "startupScene": "assets/maps/main.json",
  "modules": { "narrative": false, "ai": false }
}
```

Everything defaults to **enabled**. A disabled module:

- registers none of its component types — scenes skip them with a `[warn]`,
  the Add Component menu doesn't offer them;
- keeps its systems off — physics doesn't step, audio doesn't initialize, the
  navmesh won't bake.

Toggle them live in the editor: **Tools > Modules & Plugins** (persists to the
manifest; already-loaded scene components of a disabled type go away on the
next scene load). Packaged games carry the flags in `game.json`.

## Project plugins (drop features in)

A plugin is a folder under `Plugins/` with a manifest, sources, and a compiled
DLL:

```
Plugins/MyFeature/
  plugin.json          { "description": "...", "enabled": true, "sourceDir": "Source" }
  Source/
    module_entry.cpp   the exports (copy the block below)
    MyComponent.h/.cpp same SDK as Source/ scripts (see scripting-cpp.md)
  Binaries/MyFeature.dll   built by Compile in the Modules & Plugins panel
```

`module_entry.cpp` — copy verbatim (only the file must exist; nothing to edit):

```cpp
#include <ae.h>

extern "C" __declspec(dllexport) int GameModule_AbiVersion() {
    return AE_ABI_VERSION;
}
extern "C" __declspec(dllexport) void GameModule_RegisterAs(ae::ComponentRegistry& r,
                                                            int moduleId) {
    ae::detail::runPendingRegistrations(r, moduleId);
}
```

Components declared with `AE_COMPONENT`/`AE_REGISTER_COMPONENT` in the
plugin's sources register under the plugin's own module id, so the engine can
unload/reload the plugin wholesale.

Lifecycle (all in **Tools > Modules & Plugins**):

- **Compile** — builds `Binaries/<Name>.dll` with the same scaffold as game
  scripts, then auto-reloads the plugin.
- **Reload** — snapshot the scene, unload the DLL, load the fresh one,
  restore the scene. Safe while the editor runs.
- **Enable checkbox** — persisted to `plugin.json`; disabled plugins never
  load (their component types skip on scene load, like a disabled module).

At startup (editor and runtime) every enabled plugin with a binary loads
automatically, after the game module. Packaging copies each plugin's manifest
and DLL into the shipped game.

This project ships a worked example: `Plugins/OrbitFx` (a `PulseLight`
component that pulses a scene light — check `assets/maps/sample.json` for the
entity using it).
