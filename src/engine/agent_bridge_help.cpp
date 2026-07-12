#include "agent_bridge_help.h"

namespace ae {

const char* agentBridgeHelpJson() {
    return R"HELP({
"service":"aether-agent-bridge",
"auth":"send the bridgeToken from %APPDATA%/AetherEngine/pulse.json as the x-aether-token header",
"notes":["entity selectors: pass guid, name, or id at the top level of params",
"mutations made during Play affect the live session only and are discarded by play.stop",
"paths are project-relative (assets/...) or absolute"],
"methods":{
"status":"editor snapshot: project, scene, playing, entity count, fps",
"bridge.help":"this document",
"registry.components":"every registered component type (what component.set accepts)",
"registry.assets":"named procedural meshes and texture sets",
"world.get":"the whole scene as map JSON (same format as assets/maps/*.json)",
"entity.list":"flat entity list: guid, id, name, parentGuid, active, componentTypes",
"entity.get":"one entity subtree as prefab-shaped JSON (selector: guid|name|id)",
"entity.spawn":"spawn via params.kind (empty|cube|sphere|plane|torus|light|camera|trigger), params.prefab (path), params.entities (scene-shaped array), or bare params.name; optional parent, name, position[3], rotation[4 quat], rotationEuler[3 deg], scale[3]",
"entity.set":"update an entity: rename, active, position, rotation, rotationEuler, scale, parent (selector or null to unparent)",
"entity.destroy":"destroy an entity subtree (selector)",
"component.set":"add-or-update components from JSON: params.component = {type,...} or params.components = [...]; same field names as scene files (see Docs/reference)",
"component.remove":"remove the first component whose type == params.type",
"scene.new":"replace the world with the starter scene (edit mode only)",
"scene.save":"save to params.path or the current map path (edit mode only)",
"scene.load":"load a map (edit mode only)",
"scene.environment":"get, or set with params sunDir[3] / skyIntensity",
"play.start":"enter Play (PIE snapshot taken first)",
"play.stop":"stop Play and restore the pre-play world",
"play.pause":"params.paused = true|false",
"logs.get":"engine log since params.since (default 0), params.max (default 200); returns next cursor",
"logs.clear":"clear the engine log",
"viewport.screenshot":"render the viewport now and write a BMP to params.path (default <project>/bridge_screenshot.bmp)",
"camera.get":"editor freecam pose",
"camera.set":"move the editor freecam: position[3], yaw (deg), pitch (deg)",
"compile.scripts":"kick off the async C++ game-module build (hot-reloads on success)",
"compile.status":"{compiling, lastOk}",
"nav.bake":"bake the navmesh from static colliders",
"edit.undo":"undo the last edit",
"edit.redo":"redo"
}})HELP";
}

} // namespace ae
