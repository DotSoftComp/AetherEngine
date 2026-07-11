// Aether Engine — runtime save-games (checkpoints).
//
// A save file captures the RUNNING game, not the authored map: the full world
// snapshot (entities, components, transforms — the same serializer that powers
// PIE and undo) plus the runtime state the scene format deliberately omits —
// story flags, mission/objective states, script-graph variables, and animator
// parameters, keyed by entity GUID.
//
// Gameplay saves/loads with zero code: the SaveGame / LoadGame script nodes
// write to <project>/Saves/<slot>.json. Loading is REQUESTED (World::
// requestLoadGame) and executed by the app layer between frames — a script
// can't destroy the world it is executing inside. Note the standard checkpoint
// semantics: after a load, components restart (OnStart fires again), so gate
// one-time sequences behind story flags.
#pragma once
#include <string>

namespace ae {

class World;
class AssetLibrary;

bool saveGame(World& world, AssetLibrary& assets, const std::string& path);
bool loadGame(World& world, AssetLibrary& assets, const std::string& path);

// Runs any save/load requests queued on the World (script nodes, menus).
// Call once per frame after World::update; returns true if a load replaced
// the world.
bool processSaveRequests(World& world, AssetLibrary& assets);

} // namespace ae
