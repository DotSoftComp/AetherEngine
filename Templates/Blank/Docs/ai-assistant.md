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

## How it works (what the panel visualizes)

```
prompt -> team -> knowledge -> planner -> specialists -> review
```

- **Team** — three persistent PulseLABS personas are found or created:
  *Aether Planner* (technical producer), *Aether Coder* (gameplay programmer),
  *Aether Designer* (technical designer). They are real agents with memory on
  the backend side.
- **Knowledge** — this project's `Docs/` (including the generated component and
  script-node reference) is ingested into Pulse Cortex and linked to the
  agents. From then on they answer **grounded in this engine's real API** —
  the "RAG: N chunks" line on each agent card shows exactly which docs a
  response used (hover for the citations).
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

## Notes

- Backend calls run on a worker thread — the editor never blocks.
- Assistant state (persona ids, knowledge flag) lives in
  `Intermediate/AiAssist/state.json`; delete it to re-create the team or
  re-ingest docs (e.g. after regenerating the doc reference).
- Applied files are ordinary project files: verify them with the usual
  headless loop (see [README.md](README.md)).
