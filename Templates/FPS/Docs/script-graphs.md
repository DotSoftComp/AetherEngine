# Script graphs (visual scripting)

Script graphs are Aether's blueprint system: event-driven gameplay logic stored
as **plain JSON** in `assets/scripts/*.json` and attached to an entity with a
`ScriptGraph` component (`{ "type": "ScriptGraph", "graph": "assets/scripts/x.json" }`).
The editor has a node canvas (double-click the file in the Content Browser),
but the format is designed to be authored directly — by hand or by an agent.

The complete node list with every pin and parameter lives in
[reference/script-nodes.md](reference/script-nodes.md) (generated — trust it
over your memory).

## File schema

```json
{
  "scriptGraph": 2,
  "variables": [
    { "name": "score", "t": "Float", "f": 0 },
    { "name": "home",  "t": "Vec3",  "v": [0, 0, 0] }
  ],
  "nodes": [
    { "id": "start", "type": "OnStart", "exec": ["say"], "in": [], "x": 40,  "y": 40 },
    { "id": "say",   "type": "Log",     "exec": [""],
      "in": [ { "t": "String", "s": "hello from a graph" } ],   "x": 240, "y": 40 }
  ]
}
```

- `scriptGraph`: format version, always `2`.
- `variables`: per-instance variables, reset on (re)start. `t` is one of
  `Float`, `Vec3`, `Bool`, `String`, `Entity`; the value key matches the type
  (`f`, `v`, `b`, `s`). Read/write them with the `GetVar`/`SetVar` nodes
  (variable name goes in the node's `p` param).
- `nodes`: the graph. Each node:
  - `id` — any unique string. Used by `exec` and `in` links.
  - `type` — a node type from the reference. Unknown types log a warning and
    do nothing.
  - `exec` — one entry per exec-out pin, in pin order: the `id` of the next
    node to run, or `""` for "stop here". Omit for pure nodes.
  - `in` — one entry per data-in pin, in pin order. Each entry is either
    - a link: `{ "from": "<node-id>", "out": <output-pin-index> }`, or
    - a literal: `{ "t": "Float", "f": 1 }`, `{ "t": "Vec3", "v": [x,y,z] }`,
      `{ "t": "Bool", "b": true }`, `{ "t": "String", "s": "text" }`.
    Omitted/missing entries use the pin's default.
  - `p` (string) / `n` (number) — per-node parameters (flag names, asset
    paths, radii...). The reference says which nodes use them and for what.
  - `x`, `y` — canvas position. Cosmetic only; any numbers work. When
    generating nodes, space them ~200 apart in x and ~120 in y per row so the
    graph stays readable in the editor.

## Execution model

- **Events** (`OnStart`, `OnUpdate`, `OnUIButton`, `OnAction`, `OnPlayerNear`,
  `OnKey`, `OnFlag`) have no exec-in. When one fires it spawns a token that
  runs node-to-node along `exec` links until it hits `""` or a `Delay`
  (which parks the token for N seconds without blocking anything else).
- **Pure nodes** (no exec pins — math, `GetVar`, `GetData`, `AIArrived`,
  comparisons...) run on demand whenever a downstream input pulls their
  output. They never need to be in an exec chain.
- Values convert permissively between pin types (Float 0/1 <-> Bool, Float ->
  Vec3 splat, anything -> String), so wiring a Float into a Bool input works.
- `If` routes the token to exec-out 0 (`true`) or 1 (`false`).
- `Sequence` spawns parallel tokens on each connected pin. `ForLoop` runs its
  `body` chain once per index — wire the *end* of the body back into the
  ForLoop node to iterate.
- Scripts tick after physics/controllers each frame; events fire in node-array
  order within one component.

## Authoring workflow for an agent

1. Read [reference/script-nodes.md](reference/script-nodes.md) for exact pins.
2. Write the JSON (small graphs > one giant graph; one file per behavior).
3. Attach it: add `{ "type": "ScriptGraph", "graph": "assets/scripts/<file>.json" }`
   to an entity's `components` in the map, or set the field in the editor.
4. Verify headless: add a `Log` node at the key point, then
   `AetherRuntime.exe --project . --frames 300` and grep stdout for
   `[Script] <your text>`. Unknown node types print
   `[warn] ... unknown node type` — an empty run log means the event never
   fired, not that the file failed to load.

Worked examples in this project: `assets/scripts/orb.json` (OnUpdate math
loop, data-table reads, UI button + impulse), `assets/scripts/guard.json`
(NavAgent patrol with arrival branching), `assets/scripts/player.json`
(input axes -> character movement + animation parameter).
