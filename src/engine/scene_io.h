// Aether Engine — scene/map serialization. The whole World (environment,
// entity hierarchy, components) round-trips to human-readable JSON so maps
// are real assets: File > Save/Open in the editor, `--game --map <file>` in
// the standalone game. Assets are referenced through the AssetLibrary by
// name (procedural meshes / texture sets) or project-relative path (glTF).
#pragma once
#include <string>

namespace ae {

class World;
class Entity;
class AssetLibrary;

bool saveWorld(const World& world, AssetLibrary& assets, const std::string& path);
// Replaces the World's current contents (clear + rebuild).
bool loadWorld(World& world, AssetLibrary& assets, const std::string& path);

// In-memory equivalents (no file I/O, no logging): the exact same JSON as
// save/loadWorld, used for editor undo/redo snapshots and any transient
// round-trip. deserializeWorld replaces the World's contents.
std::string serializeWorld(const World& world, AssetLibrary& assets);
bool deserializeWorld(World& world, AssetLibrary& assets, const std::string& json);
// Same, from an already-parsed JSON object (save-game files embed the world).
struct JsonValue;
bool deserializeWorldJson(World& world, AssetLibrary& assets, const JsonValue& root);

// Deep-copies an entity (components + children) next to the original.
Entity* duplicateEntity(World& world, AssetLibrary& assets, Entity* src);

// ---- prefabs: reusable entity assets (assets/entities/*.json) ----
// A prefab is one entity subtree (root + descendants + components) saved as an
// asset. Instantiating one spawns fresh copies with new Guids (so many
// instances coexist), remapping intra-prefab references to the new copies.
bool saveEntityPrefab(const Entity& root, AssetLibrary& assets, const std::string& path);
Entity* instantiatePrefab(World& world, AssetLibrary& assets, const std::string& path,
                          Entity* parent = nullptr);

} // namespace ae
