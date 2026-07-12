// Aether Engine — the agent bridge's self-description, as one JSON document.
//
// Single source of truth for what the bridge can do: the live editor returns
// it verbatim from the `bridge.help` method (editor/bridge_commands.cpp), and
// AetherDocGen embeds it in Docs/reference/agent-bridge.md so agents working
// from a project repo (and the PulseLABS knowledge base) see the same list.
// Lives in AetherCore.dll so both consumers share it. Add a line here whenever
// bridge_commands.cpp gains a method.
#pragma once

namespace ae {

const char* agentBridgeHelpJson();

} // namespace ae
