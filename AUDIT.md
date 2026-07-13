# Aether Engine — Audit, Positioning & Roadmap

*Rewritten 2026-07-11 after a full-source audit (~25,400 lines of C++ +
~2,000 lines of GLSL verified against every claim below).*

This document is three things: an honest inventory of what the engine **is**
(Part 1), a sober comparison against Unity and Unreal (Part 2), the strategy
that follows from it (Part 3), and the detailed to-do list that executes the
strategy (Part 4).

---

## Positioning

**Aether is the first agent-native game engine.** The pitch to an indie dev is
not "a lighter Unity" — it is:

> **You + an AI agent = a game studio.** Describe a mechanic; the agent
> authors the blueprint JSON, wires the scene, runs the game headless, reads
> its own logs, screenshots the result, and iterates — while you art-direct.
> Every change is a git diff you can review.

Three pillars, in priority order:

1. **Agent-native** — every asset is hand-editable JSON; docs are generated
   from live registries and ship inside the game repo; a headless verify loop
   lets an agent prove its own changes. *(The differentiator — see Part 3.)*
2. **Modular** — engine features are per-project toggleable modules; systems
   live behind facades (Jolt behind `ae::PhysicsWorld`, GL behind `ae::rhi`);
   projects extend the engine via hot-reloadable plugins with their own
   registry groups.
3. **Easy for everyone** — node canvases with hot reload for every authoring
   domain; a one-class C++ SDK (`reflect()` + `AE_REGISTER_COMPONENT`) where
   serializer, inspector, prefabs, undo and docs adapt automatically.

Aether is **genre-neutral**: the narrative stack (dialogue graphs, QTEs,
missions) is one optional module among peers, kept as a showcase toolkit.

---

## Part 1 — State of the engine (verified)

### The load-bearing number

The **entire engine is ~25K lines of C++** (Godot ≈ 2M, Unreal ≈ 15M+).
Breakdown: editor 7.0K · engine core 4.7K · render 4.2K · core 1.5K ·
script 1.3K · ai_assist 1.0K · narrative 1.0K · rhi 0.9K · ui 0.9K ·
physics 0.65K · audio/ai/hub/gl/scene ≈ 1.9K. This compactness is not trivia:
it is why the whole engine fits in an agent's context window and a solo dev's
head, and it is achieved by two architectural bets that repeat everywhere:

- **Reflection-driven components** (`engine/reflect.h`): one `reflect()`
  function per component drives scene save/load, duplication, prefab GUID
  remapping, the Details inspector, undo snapshots, and the generated docs.
  Adding a component touches nothing else.
- **Self-describing registries**: script nodes (~82), material nodes (~20),
  components, engine modules and plugins are each *one registry entry* that
  simultaneously feeds the runtime, the serializer, the canvas editor and
  `AetherDocGen`. Adding a script node is ~5 lines.

### What's built — feature inventory

Everything below exists in the tree today and was spot-verified.

**Rendering** (`src/render/`, `src/rhi/`, `shaders/` — from-scratch GL 4.5 DSA)
- Deferred PBR: Cook–Torrance GGX, metallic/roughness, normal mapping,
  emissive, alpha cutout; pre-integrated SSS skin (d'Eon LUT, dual-lobe GGX,
  transmission).
- Nishita atmosphere ray-marched to a cubemap → live-rebuilt diffuse/specular
  IBL when the sun moves.
- 4 stabilized cascaded shadow maps (runtime-adjustable count), 24-sample
  SSAO, HDR MRT pipeline, height fog, Jimenez bloom, ACES + vignette +
  dither + FXAA.
- Point / spot / directional lights with range (no local shadows yet).
- Forward transparency pass: painter-sorted, fully lit (sun+IBL+locals+fog),
  gets bloom/tonemap for free; glTF `alphaMode: BLEND` auto-imports.
- **GPU instancing**: identical draws auto-batch per frame into
  `glDrawElementsInstanced` (SSBO matrices; shadow pass batches by mesh
  alone); per-object draw distance. Measured 877→2 draw calls,
  pixel-identical A/B.
- CPU particles: cone emitters, size/color-over-life (HDR → free bloom
  glow), additive/premultiplied, world-space trails.
- Debug draw: colored 3D line X-ray overlay (colliders, light ranges,
  navmesh); always-on double-buffered GPU pass timers.
- Frustum + oriented-AABB culling; frame stats in the status bar.
- **RHI phases 1+2 done**: zero GL calls outside `rhi_gl.cpp` (editor-only
  exceptions: ImGui backend, WGL creation). Textures, FBOs, buffers,
  streams, timers, shaders, draws all go through `ae::rhi`; pixel-identical
  at every milestone. Phase 3 (shader cross-compile + D3D12/Vulkan backend)
  is the remaining real work.

**Editor** (`src/editor/` — Dear ImGui docking + ImGuizmo, vendored)
- UE-style docked workspace, custom borderless chrome, persistent layout.
- Viewport (FBO panel): freecam, LMB ray picking, gizmos with snap, drag-drop
  spawning; **Edit-mode render caching** (idle editor: 3.5→170 fps on iGPU).
- Outliner (search, drag-reparent preserving world transform), reflection-
  driven Details inspector (entity-ref dropdowns, asset combos, drop
  targets), Content Browser (typed tiles, double-click routing to the right
  graph editor), Output Log, World Settings, Profiler panel.
- **Prefabs** (subtree assets, fresh GUIDs + intra-ref remap on instantiate),
  **snapshot undo/redo** (128 steps, gesture-aware), **true PIE** (serialize
  → play → restore, HUD composited into the viewport, mouse remapped).
- **Six graph/data editors**, all with Ctrl+S hot reload (live during Play):
  visual scripting (typed pins, variables, functions, searchable palette),
  material graphs, anim state machines (live param values, active-state
  highlight), dialogue graphs (lights up live like Detroit's flowchart),
  UI designer (anchor/pivot presets, virtual-screen preview), data-table
  spreadsheet.
- Input Map panel, Missions panel, Modules & Plugins panel (live
  enable/compile/reload), AI Assistant panel.

**Gameplay & systems** (`src/engine/`, `physics/`, `ai/`, `audio/`, `script/`, `ui/`)
- Entity/component World with GUID-based `EntityRef` (self-healing, prefab-
  safe), camera director (named shots, cut/blend requests).
- **Visual scripting v2** (Blueprint-class): exec + typed data links,
  variables, functions with call stack, If/ForLoop/Sequence/Delay/Once, ~82
  nodes (events, entity, physics, AI, math, game, UI, debug, data).
- **Physics**: Jolt v5.5 behind a pimpl facade — rigid bodies, box/sphere/
  capsule colliders, static/dynamic/kinematic, triggers, raycasts, grounded
  probe, CharacterController; component-reconciled every step (no leaks).
- **Navigation**: Recast/Detour navmesh baked from static colliders
  (agent-aware settings, auto-bake on first use), NavAgent locomotion
  through rigid-body velocity, AIMoveTo/AIStop/AIArrived nodes, editor bake
  + overlay.
- **Audio**: XAudio2, 3D voices with rolloff + pan, AudioSource component,
  drag-drop clip assignment, listener from the active camera.
- **Input**: XInput gamepad + rebindable action/axis map (`assets/input.json`),
  keyboard/mouse/pad sources composable per action, script nodes + panel.
- **Animation**: glTF skinning (GPU, 128-joint UBO), AnimGraph state machines
  with param-guarded transitions and crossfade blending, gameplay bridge via
  SetAnimParam (C++ and script nodes).
- **Game UI**: UIDocument widget trees (Panel/Label/Button/ProgressBar),
  anchor+pivot+offset layout, story-flag data binding, OnUIButton script
  events — designer + graph, zero code.
- **Persistence**: save-game checkpoints (world snapshot + flags + missions +
  script vars + anim params), SaveGame/LoadGame nodes, deferred safe loads.
- **Data tables**: rows × typed columns, GetData/DataRowCount/DataRowName
  nodes, spreadsheet editor with hot reload into live Play.
- **Missions & story flags**: objectives (Reach/Flag), HUD, panel; flags as
  the shared branching state across dialogue/missions/scripts/UI.
- **Narrative module**: dialogue scenes (Line/Choice/QTE/End, per-node camera
  cuts + flag writes), DialoguePlayer, proximity triggers, choice prompt +
  QTE UI (Tap/Hold/Mash/Sequence), mission HUD.

**Pipeline, projects & platform**
- Import: glTF 2.0 (.glb/.gltf, hand-written), OBJ/MTL (hand-written), FBX
  (vendored ufbx) — all through one ModelBuild scaffold. **BC1/BC3 texture
  compression with a content-addressed disk cache** (8× less VRAM, 2.7×
  faster warm startup, measured).
- Projects: `.aeproj` manifest, AetherHub launcher (installs, templates,
  recent projects), packager (`--package Dist`), `game.json` quality tiers +
  auto-tier on iGPUs.
- **Engine modules**: 8 named built-ins toggleable per project (components
  never register, systems stay off). **Plugins**: `Plugins/<Name>/` native
  DLLs, own registry group, async compile + snapshot-reload from a panel.
- C++ game modules: `<ae.h>` SDK, hot-copied DLL, editor-triggered compile.
- **AI-first docs**: `Docs/` in every template (scene/script/data schemas,
  C++ SDK, modules guide) + `reference/` **generated from live registries**
  (every component's exact JSON keys/defaults, every node's pins) +
  `AGENTS.md`/`CLAUDE.md` orienting an agent, including the headless verify
  loop.
- **Verification culture**: 6 headless smoke exes (physics, undo, input,
  save, nav, AI pipeline) + `--frames/--screenshot/--resave/--play/--compile`
  flags on the main exes; renderer changes A/B'd pixel-identical.
- **AI Assistant**: editor panel over the PulseLABS v1 API — personas,
  Cortex RAG on the project's own Docs/, multi-plan pipeline, per-file
  proposals with explicit apply. (Verified against a schema-faithful mock;
  live-backend run pending.)

### The hard truths

1. **Windows-only, all the way down.** Win32 window/chrome, WIC images, GDI
   fonts, XAudio2, XInput, WinHTTP, `CoCreateGuid`, WGL + GL 4.5. The RHI
   split is real but `rhi.h` is a GL-shaped ~250-line interface that still
   speaks GLSL; the D3D12/Vulkan port is the hard part and it's still ahead.
   macOS — where a large share of indies live — has no GL 4.5 at all.
2. **Rendering ceiling.** No GI beyond IBL, no TAA/temporal upscaling, no
   SSR, no local-light shadows, no LOD chains, no decals/volumetrics/DoF/
   motion blur/auto-exposure. Fine for stylized indie games; visible next to
   UE5 screenshots.
3. **World scale.** One hand-placed JSON map at a time; no terrain, no
   streaming/partition. Caps games at room-to-neighborhood scale.
4. **Missing tiers.** No networking. No managed scripting tier (C#/Lua)
   between JSON graphs and native C++. FBX skinned animation doesn't import
   (rigged characters must be glTF).
5. **Ecosystem = zero.** No asset store, tutorials, forum answers, console
   porting path, or hiring pool. No amount of code closes this quickly —
   which is exactly why the strategy below doesn't fight on breadth.

---

## Part 2 — Honest comparison vs. Unity and Unreal

| Dimension | Aether | Unity | Unreal |
|---|---|---|---|
| Renderer quality | AA-level PBR; no GI/TAA/SSR | HDRP ≈ AAA; URP flexible | Best-in-class (Lumen, Nanite) |
| Editor UX | Genuinely UE-like: docking, PIE, undo, prefabs, gizmos, 6 graph editors | Mature | Mature, heavy |
| Visual scripting | Blueprint-class v2, hot-reload during Play | Weak (widely disliked) | Blueprint = the gold standard |
| Text/native scripting | C++ hot-reload DLLs; 1-class integration | C# — its killer feature | C++, slow iteration |
| Asset formats | 100% human-readable JSON, git-diffable | YAML scenes (GUID soup) + binary import DB + .meta | Binary .uasset — opaque, merge-hostile |
| Agent workability | **Native**: open formats + generated in-repo docs + headless verify | Poor: needs the editor alive; formats undocumented | Worst: content unreadable to agents |
| Iteration | Everything hot-reloads; JSON needs no compile | Domain-reload pain | Legendary compile times |
| Physics / nav / audio | Jolt / Recast / XAudio2 (solid, v1 limits) | PhysX + mature everything | Chaos + mature everything |
| World scale | Single small map | Large scenes, DOTS | World Partition, open worlds |
| Platforms | Windows + GL only | ~25 platforms | All majors + consoles |
| Ecosystem | None | Enormous | Enormous |
| Source | 25K lines, fully readable | Closed core | 15M lines, open but impenetrable |
| Terms | Yours | Runtime-fee trust damage lingers | 5% royalty |

**The sober read:** Aether will never out-Unity Unity on breadth or out-Unreal
Unreal on graphics, and every indie engine that fought that war died (Stride,
NeoAxis, dozens more). Godot won its niche by being open, light and pleasant —
not feature-complete. The differentiator therefore cannot be a feature the
incumbents can copy in a point release. It must be **structural**: something
they cannot follow without breaking their own ecosystems.

---

## Part 3 — The differentiator: agent-native

Aether's architecture already *is* what an AI coding agent needs, and no
incumbent can retrofit it:

- **Every asset is plain JSON** an agent can read, write, and git-diff. In
  Unity an agent can't safely touch a scene (GUIDs into an import database
  that only exists while the editor runs); in Unreal it can't touch content
  at all.
- **Docs that can't rot** — generated from the same registries the engine
  runs on, shipped inside the game repo where the agent works, with exact
  JSON keys, pins and defaults.
- **A headless verify loop** — run N frames, grep `[Script]` logs,
  screenshot, resave round-trip — so an agent **proves its change worked
  with no human clicking anything**. Neither incumbent has a sanctioned
  equivalent; their editors *are* the runtime.
- **Self-describing registries** mean an agent can extend the engine itself
  (component = one class + `reflect()`; node = ~5 lines) and the editor,
  serializer and docs adapt automatically.
- **25K lines** means the whole engine fits in an agent's context window —
  the "small codebase" weakness is the moat's foundation.

Unity and Unreal cannot follow: binary/opaque formats, editor-bound import
pipelines, and two decades of backward compatibility are structural. The
beachhead audience — solo devs already living in Claude Code / Cursor —
already exists and is growing.

**Supporting differentiators** (fold into the same story, don't lead with):
hot-reload-everything iteration (edit a data table *during Play*), the
built-in narrative toolkit (a genre kit no engine ships), and no-black-box
hackability.

---

## Part 4 — Roadmap

Ranked by strategy: **A** cements the differentiator, **B** removes adoption
blockers, **C** closes expected-feature gaps, **D** is scale/breadth,
**E** long tail. Within a tier, roughly in order.

### Tier A — Cement the agent-native moat (do first)

| # | Item | Detail |
|---|---|---|
| A1 | **Agent bridge for the live editor/engine** ✅ *(v1 shipped 2026-07-11)* | Drive a running editor: spawn/modify entities (reflection-driven `component.set`), start/stop Play, query world/logs, viewport screenshots, camera control, compiles/bakes, undo/redo. Localhost HTTP + token (`editor/agent_bridge.h`, `bridge_commands.cpp`); self-describing via `bridge.help` / `registry.components`. **Deliberately PulseLABS-gated** (shared token in `pulse.json`) — the strategy is agents *through PulseLABS*, local or cloud, not an open plug-any-agent port; opening it later is a policy change, not a rewrite. Remaining: PulseLABS dev-team tool-calling loop over the bridge (ties into A5). |
| A2 | **The proof video** — *demo asset shipped 2026-07-12, recording pending* | `tools/proof_demo/`: `proof_run.py` drives a live editor through the whole claim in one ~90s take (build scene → write git-diffable script JSON **with a planted typo** → read the precise `file:node:pin` error from logs → self-fix → screenshot → Play → prove the script ran from its own logs → `--verify`), plus `STORYBOARD.md` (shot list, narration, Game Bar/OBS recording guide, long-cut + CI-cut variants). Verified end-to-end incl. the screenshots. Remaining: press record. |
| A3 | **Genre starter kits** (platformer, top-down, FPS) — *FPS shipped 2026-07-12* | Matter double: onboarding for humans *and* few-shot examples for agents. Ship as Hub templates with full Docs/. **`Templates/FPS`**: mouse-look + `FirstPersonCamera` engine rig (new built-in), hitscan shooting with ammo/reload from a data table, 6 destructible targets via the cross-graph flag pattern, mission + bound HUD, modules trimmed (`narrative/ai/animation` off), `Docs/starter-kit-fps.md` extension recipes, passes `--verify`. Platformer + top-down remain. |
| A4 | **Agent-grade error reporting** ✅ *(shipped 2026-07-12)* | `AetherRuntime --verify` = resave round-trip + N frames + log scan, exits nonzero and ends stdout with one `VERIFY PASS\|FAIL {json}` line (errors, schema warnings, script-log count). Script-graph loader now validates every link with `file:node:pin` messages (unknown node types, dangling exec/data links, out-of-range outputs); scene loader names the entity on unknown component types. Docs/AGENTS.md lead with the one-command loop. |
| A5 | **Live PulseLABS run** of the AI Assistant — *engine side shipped 2026-07-12* | Multi-turn **proposal refinement** (feedback re-enters each specialist's same session; revised files replace proposals by path), **auto-compile of applied C++** proposals (hooks the editor's script build → hot reload), and **persona→bridge tool-calls** (`bridgeCalls` in specialist responses, listed in the panel, user-approved execution against the live editor — verified end-to-end: mock persona spawned an entity in a running editor). Remaining: routine runs against the live PersonAi backend (config-only). |
| A6 | **Screenshot diffing helper** ✅ *(shipped 2026-07-12)* | `AetherRuntime --compare ref.bmp [--psnrmin N]` re-captures and prints `COMPARE PASS\|FAIL {json}` (psnr/meanAbs/diffPct); failures write an amplified `*_diff.bmp` heatmap and exit nonzero; integrates with `--verify` (a compare failure fails verification). Deterministic fixed-dt capture ⇒ unchanged renders are bit-identical (PSNR 99). |

### Tier B — Adoption blockers

| # | Item | Detail |
|---|---|---|
| B1 | **RHI phase 3** | Shader cross-compilation (GLSL is the last GL-ism in the interface), context/swapchain behind the RHI, redundant-state caching, then `rhi_d3d12.cpp` or `rhi_vulkan.cpp` against the existing header. |
| B2 | **Cross-platform core** | Behind B1: abstract window/input/audio/image/font/HTTP (currently Win32/WIC/GDI/XAudio2/XInput/WinHTTP). Linux first (cheap after Vulkan), then macOS (Metal or MoltenVK). "Windows-only" is the one table row that makes the target audience close the tab. |
| B3 | **FBX skinned animation import** (ufbx exposes it) — rigged characters currently must be glTF. |
| B4 | **Local light shadows** (spot first — single map; point via cube). |
| B5 | **Mesh LOD chains** (authored + optional auto-decimate) riding the existing instancing/distance-cull path. |
| B6 | **UI: Image widget + per-widget fonts/scaling + keyboard/gamepad focus navigation** — the current four widgets can't build a real menu. |
| B7 | **Script debugging**: per-node execution overlay on the canvas during Play (fires/values on wires), breakpoints; function argument/return pins (v2 functions are procedures). |
| B8 | **Input v2**: multiple pads + per-player maps; migrate built-in behaviors (CameraController) off raw keys. |
| B9 | **Save v2**: preserve physics velocities; save-slot browser UI (screenshot + timestamp). |
| B10 | **Engine auto-update via the Hub** ✅ *(shipped 2026-07-12)* | Release pipeline: push a `v*` tag → `.github/workflows/release.yml` builds, runs the headless smokes, stages the install layout (`tools/make_install.ps1`: exes + shaders + Templates + `SDK/` headers/lib + relocatable `engine.json`) and publishes `AetherEngine-<ver>-win64.zip` as a GitHub Release. The hub checks `releases/latest` async on launch (`hub/engine_update.cpp`), shows an Update banner, downloads with progress (`httpDownloadToFile`), extracts via bsdtar and installs **side-by-side** under `%LOCALAPPDATA%/AetherEngine/Versions/<ver>` — existing installs/projects untouched (projects pin engine versions), one-click "Restart Hub in new version". Remaining: code-signing the binaries (unsigned exes trip AV heuristics — observed with McAfee) and delta/partial updates. |

### Tier C — Expected-feature gaps (v1 limits to close)

| # | Item | Detail |
|---|---|---|
| C1 | **Animation v2** ✅ *(shipped 2026-07-12)* — Per-instance poses: `ModelPose` (node TRS + palettes + morph state) moved out of the shared `Model` asset into ModelComponent, so any number of entities share one glTF and animate independently (all Model sampling is now const, pose-based). AnimGraph v2 (`animGraph: 2`, v1 files still load): **layers** (each its own state machine, overlaid with weight/`weightParam` + optional `maskBone` subtree mask) and **blend-space states** (`blend1d`/`blend2d`, gradient-band weighting, phase-synced motions) alongside clip states. **Root motion** on the Animator (`rootMotion` + auto/named `rootBone`, XZ to entity with loop-seam compensation). **Two-bone IK** component (`TwoBoneIK`: end bone + target/pole entities, runs in the new `onLateUpdate` phase after physics). **Morph targets**: glTF POSITION/NORMAL targets + `weights` animations, CPU-blended into per-instance dynamic meshes. Anim Graph panel: layer bar + blend-space/motion editing. ABI bumped to v2 (`onLateUpdate` virtual) — game modules/plugins recompile; template binaries rebuilt. Sample fox graph upgraded to a live Speed blend space. Remaining niceties: additive layers, IK for >2-bone chains, GPU morph blending. |
| C2 | **Particles v2** ✅ *(shipped 2026-07-12, GPU sim staged)* — Sprite textures on emitters (`texture` path, sRGB, cached via the asset library; blank keeps the procedural soft disc), **flipbook animation** (`flipbookCols/Rows` grid + `flipbookFps`, 0 = one pass over the particle's lifetime; cell UVs computed CPU-side), **per-particle rotation** (`spin` deg/s ± `spinJitter`, `randomRotation` start roll; billboard basis rolled in the camera plane), and **soft depth fade** (`softFade` meters — fragment fades against the opaque depth buffer, same attached-depth-sample pattern as the composite pass; kills hard clip lines in smoke/fog). Verified visually + A/B `--compare` (fade on/off vs an intersecting wall). *GPU sim for big counts deliberately staged:* the RHI has no compute path yet — do it after B1's RHI phase 3 so it lands portable instead of GL-only (CPU sim measured ~0.2 ms at current template loads; cap is 10k/emitter). |
| C3 | **Material graphs v2** ✅ *(shipped 2026-07-12)* — **Normal-map output**: the Output node gained a tangent-space `Normal` pin (new `NormalMap` texture node decodes + applies strength; generated TBN application spliced at a new `//__MG_NORMAL__` marker in pbr.frag, so graph materials get bumped lighting in both passes). **Texture slots raised 4 → 8** (bindings 13..20; slots shared across subgraph expansions). **Subgraphs/functions**: a `Subgraph` node references another `assets/materials/*.json` and inlines it at compile time — `SubInput` nodes (pin index 0-3 = caller's A-D) are the arguments, one `SubOutput` the return; nested subgraphs cached per compile, recursion/depth guarded (magenta on cycle or missing file). Codegen API reshaped (`MGGenerated` + `MGSubLoader`); existing graphs untouched (Normal pin appended last, loader pads). Verified: subgraph-tinted + bump-mapped wall renders, normal on/off A/B compare (61% px), 6-texture graph compiles past the old cap. |
| C4 | Navigation: tile cache + dynamic obstacles (rebake-free), DetourCrowd avoidance, render-mesh bake input, behavior trees + perception. |
| C5 | Data tables: per-column type enforcement, Vec3/asset-ref/color cell types, CSV import/export. |
| C6 | Import pipeline: import-settings sidecars, BC5 normal maps, mip streaming, dependency tracking/hot-reimport. |
| C7 | Profiling: CPU sampling profiler, stat capture/export (chrome-tracing JSON), thick debug lines. |
| C8 | Rendering quality ladder (in rough value order): TAA → auto-exposure → SSR → baked lightmaps or DDGI-style GI → decals → volumetrics → DoF/motion blur → post-process volumes. |
| C9 | Audio v2: streaming (music), OGG import, mixer buses/effects, script nodes for volume/pitch. |

### Tier D — Scale & breadth

| # | Item | Detail |
|---|---|---|
| D1 | Terrain (heightmap + splat materials + foliage scattering) — unlocks whole genres. |
| D2 | Level streaming / world partition; multiple concurrent scenes (additive load). |
| D3 | Networking (client/server replication of components + script events; co-op first, not MMO). |
| D4 | Job system / multithreading (render command generation, particle sim, navmesh bake are single-threaded today). |
| D5 | Console/mobile paths (long-term; follows B1/B2 and real demand). |

### Tier E — Long tail

Engine modules as separate DLLs · editor-extension API for plugins ·
inter-plugin dependencies · live module toggle without scene reload ·
in-editor help browser · localized docs · asset marketplace/sharing story.

---

## Changelog note

The previous version of this file tracked the "breadth pillars" build-out
(items 1–24: audio, physics, scripting, input, lights, transparency, undo,
particles, anim graphs, material graphs, UI, OBJ/FBX, instancing, RHI 1+2,
navigation, save games, data tables, profiling, plugins, docs, AI assistant —
all ✅ done, with texture compression). That inventory is preserved in Part 1;
its per-item "Limits:" notes seeded Tiers B/C above. Git history has the full
prior text.
