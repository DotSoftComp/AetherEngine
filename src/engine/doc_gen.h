// Aether Engine — reference-doc generator (AI-first documentation).
//
// Emits markdown generated from the engine's single sources of truth:
//   reference/components.md   — every registered component + reflected fields
//                               (name, type, default, range) via a doc visitor
//   reference/script-nodes.md — every script-graph node + its pins/params from
//                               the node registry
//   reference/agent-bridge.md — the live-editor control API (agent bridge),
//                               embedding the same JSON `bridge.help` serves
// Because the docs come from the same registries the engine runs on, they are
// always current: regenerate after adding nodes/components (AetherDocGen CLI,
// or Tools > Regenerate Doc Reference in the editor). Ships into every project
// so coding agents working in a game repo can read the full API locally.
#pragma once
#include <string>

namespace ae {

// Writes <docsDir>/reference/*.md. Requires registerBuiltinComponents() (and
// any game module/plugins whose types should appear) to have run first.
bool generateReferenceDocs(const std::string& docsDir);

} // namespace ae
