# The proof video — storyboard & recording guide

**The claim (A2):** *one prompt → playable mini-game, unedited, no human
clicking anything.* The demo shows the four things no incumbent engine can do
together: an agent **operating a live editor**, content that is **plain JSON
on disk**, an engine that **reports errors precisely enough to self-fix**, and
a **verification loop the agent runs itself**.

## Setup (one minute)

1. Open the editor on any project (`AetherEditor --project Templates/Blank`).
   Dock the **Output Log** visibly next to the viewport — the log is a
   co-star. Optionally keep a terminal and the `assets/scripts/` folder
   visible in an editor pane (VS Code) to show files appearing.
2. Start recording: **Win+G** (Game Bar) or OBS, full screen.
3. In a terminal: `python tools/proof_demo/proof_run.py`
   (pacing: `set DEMO_PAUSE=2` for slower narration; the run is ~90s at
   default pacing).

## Shot list

| # | On screen | Narration beat |
|---|---|---|
| 1 | Terminal: `/health` + `status` answer | "There's an editor running. The agent just found it." |
| 2 | Viewport wipes to the starter scene | "Fresh scene — the agent is driving, not a human." |
| 3 | Sky dims to dusk, a warm lamp + glowing orb appear one by one | "Every one of these is an API call any PulseLABS agent can make." |
| 4 | `demo_orb.json` appears in the file explorer | "Gameplay isn't opaque — it's a JSON file you could git-diff. And the agent just made a typo." |
| 5 | Output Log shows the yellow `unknown node type 'Logg' (id 'l1')` line | "The engine names the file, the node, the pin. That's what lets an agent fix itself." |
| 6 | File rewritten, component reattached, warning gone | "Fix, hot reload. No compile, no restart." |
| 7 | Camera flies to a beauty angle; screenshot path printed | "The agent takes its own screenshots — it has eyes." |
| 8 | **Play mode starts inside the editor**, orb spinning; terminal prints the green `demo: the orb is alive` log line | "It plays its own game and proves the script ran — from the logs, not from hope." |
| 9 | Terminal: closing line + `--verify` command | "And anyone — human or CI — can re-prove it with one command." |
| 10 | (Optional) run `AetherRuntime --project <proj> --map assets/maps/demo.json --verify`, end on `VERIFY PASS` | "Verify. Pass. That's the whole loop." |

## Why this is unfakeable by incumbents (closing card)

- Unity: an agent can't safely touch a scene (GUID/import DB exists only while
  the editor runs) and there is no sanctioned headless verify loop.
- Unreal: content is binary `.uasset` — unreadable, undiffable, unmergeable.
- Aether: files are the format, docs are generated from the running engine,
  and the editor itself is an API.

## Variants

- **Long cut (~10 min, the original A2 brief):** same loop but the prompt goes
  through the AI Assistant panel (PulseLABS personas plan → generate →
  bridge calls), using the FPS starter kit as the base. Record after the
  live-backend run is routine.
- **CI cut (30 s):** `DEMO_PAUSE=0 python proof_run.py` + `--verify` — the
  demo doubles as an end-to-end smoke test of the bridge.
