# AI Assistant (the PulseLABS dev team)

The editor embeds a small AI dev team (View / Tools > **AI Assistant**) backed
by a [PulseLABS](https://github.com/) server — self-hosted or cloud. One prompt
in, reviewed project files out, through a pipeline you can watch working.

## Setup

1. Run a PulseLABS backend (self-hosted default: `http://localhost:3051`) and
   create an API key in its Developer Portal.
2. In the panel: **Setup** → enter the URL + key → **Save & test**. The config
   is stored per user (`%APPDATA%/AetherEngine/pulse.json`), never in the
   project.
3. Running a **local model**? Raise "Request timeout (s)" in the same Setup
   section (`timeoutSeconds` in pulse.json, default 600) — one planning call
   can generate for many minutes. Only generation calls wait this long; a
   backend that is *down* still fails within seconds.

## How it works (what the panel visualizes)

```
prompt -> team -> knowledge -> planner -> specialists -> review
```

- **Team** — three persistent PulseLABS personas are found or created:
  *Aether Planner* (technical producer), *Aether Coder* (gameplay programmer),
  *Aether Designer* (technical designer). They are real agents with memory on
  the backend side.
- **Knowledge** — every run, the assistant diffs this project's `Docs/**/*.md`
  (including the generated reference) against what it already ingested into
  Pulse Cortex, and ingests anything **new or changed** — regenerate the
  reference and the agents pick it up on their next prompt, no manual step.
  From then on they answer **grounded in this engine's real API** — the
  "RAG: N chunks" line on each agent card shows exactly which docs a response
  used (hover for the citations).
- **Planner** — turns your prompt into **2–3 genuinely different complete
  plans**: steps, which specialist handles each, and the exact files that
  would be created. Pick one.
- **Specialists** — the chosen plan's steps fan out to the Coder (script
  graphs, data tables, C++ scripts) and Designer (scenes, UI documents,
  tuning). Each produces complete files. The "thoughts" button on an agent
  card shows its reasoning trace.
- **Review** — every proposed file is listed with a preview. Nothing touches
  the project until you click **apply**; JSON artifacts are parse-checked
  first and invalid ones cannot be applied. Model routing, PII shielding, and
  token accounting run server-side in PulseLABS.

## The agents know about the live editor

The personas are told (per call, and via the ingested
[reference/agent-bridge.md](reference/agent-bridge.md)) that a running editor
exposes the **agent bridge** — a PulseLABS-gated localhost API that can spawn
entities, set components, control Play, read logs, and screenshot the
viewport. Plans that need in-editor wiring or verification will reference
concrete bridge calls.

## Notes

- Backend calls run on a worker thread — the editor never blocks.
- Assistant state (persona ids, per-doc ingestion hashes) lives in
  `Intermediate/AiAssist/state.json`; delete it to re-create the team or force
  a full re-ingest. Ordinary doc changes need nothing — the sync is automatic.
- Applied files are ordinary project files: verify them with the usual
  headless loop (see [README.md](README.md)).
