#!/usr/bin/env python3
"""Aether proof demo — "you + an AI agent = a game studio", in one take.

Drives a LIVE Aether editor through the agent bridge exactly like a PulseLABS
agent would: builds a mini-game from nothing, writes git-diffable JSON, enters
Play, reads its own logs, breaks something on purpose, catches the precise
error, fixes it, and proves the result — no human clicks anything.

Record it:  open the editor (any project), start screen recording
(Win+G Game Bar or OBS), then run:   python tools/proof_demo/proof_run.py
See STORYBOARD.md for the narration script.
"""
import json, os, sys, time, urllib.request

PORT = 3052
PAUSE = float(os.environ.get("DEMO_PAUSE", "1.2"))  # dramatic pacing; 0 for CI

def cfg():
    p = os.path.join(os.environ["APPDATA"], "AetherEngine", "pulse.json")
    return json.load(open(p, encoding="utf-8"))

TOKEN = cfg()["bridgeToken"]

def say(line):
    print("\n\033[96m» %s\033[0m" % line, flush=True)
    time.sleep(PAUSE)

def rpc(method, params=None, quiet=False):
    body = json.dumps({"method": method, "params": params or {}}).encode()
    req = urllib.request.Request("http://127.0.0.1:%d/rpc" % PORT, body,
                                 {"x-aether-token": TOKEN,
                                  "Content-Type": "application/json"})
    r = json.loads(urllib.request.urlopen(req, timeout=60).read())
    if not quiet:
        print("  %s -> %s" % (method, json.dumps(r.get("result", r))[:110]), flush=True)
    if not r.get("ok"):
        raise SystemExit("bridge call failed: %s -> %s" % (method, r))
    return r["result"]

def logs_since(cursor, grep=None):
    r = rpc("logs.get", {"since": cursor, "max": 400}, quiet=True)
    lines = [e["text"] for e in r["entries"]]
    if grep:
        lines = [l for l in lines if grep in l]
    return r["next"], lines

# The mini-game's brain, written as a FILE — every asset is git-diffable JSON.
def spin_graph(node_type):
    # node_type is intentionally parameterized: the demo first writes a TYPO
    # ("Logg") to show the engine catching it with a precise message.
    return {"scriptGraph": 2, "variables": [], "nodes": [
        {"id": "st", "type": "OnStart", "exec": ["l1"], "in": [], "x": 40, "y": 40},
        {"id": "l1", "type": node_type, "exec": [""],
         "in": [{"t": "String", "s": "demo: the orb is alive"}], "x": 240, "y": 40},
        {"id": "up", "type": "OnUpdate", "exec": ["spin"], "in": [], "x": 40, "y": 160},
        {"id": "me", "type": "Self", "exec": [], "in": [], "x": 40, "y": 280},
        {"id": "spin", "type": "RotateYaw", "exec": [""],
         "in": [{"from": "me", "out": 0}, {"t": "Float", "f": 120}], "x": 240, "y": 160},
    ]}

def main():
    say("Is an editor alive?  GET /health ...")
    health = json.loads(urllib.request.urlopen(
        "http://127.0.0.1:%d/health" % PORT, timeout=5).read())
    print("  %s" % health)
    status = rpc("status")
    root = status["projectRoot"]

    say("Fresh scene.")
    rpc("scene.new")

    say("Set the mood: dusk sun, warm lamp.")
    rpc("scene.environment", {"sunDir": [0.2, 0.25, 0.6], "skyIntensity": 14})
    rpc("entity.spawn", {"kind": "light", "name": "Lamp", "position": [2, 3, 2]})
    rpc("component.set", {"name": "Lamp", "component":
        {"type": "Light", "color": [1.0, 0.6, 0.3], "intensity": 18, "range": 16}})

    say("A hero prop: glowing orb on a pedestal.")
    rpc("entity.spawn", {"kind": "cube", "name": "Pedestal", "position": [0, 0.5, 0]})
    rpc("entity.spawn", {"kind": "sphere", "name": "Orb", "position": [0, 1.6, 0],
                         "scale": [0.6, 0.6, 0.6]})
    rpc("component.set", {"name": "Orb", "component":
        {"type": "MeshRenderer", "mesh": "sphere",
         "material": {"baseColor": [0.3, 0.8, 1.0, 1.0], "emissive": [0.4, 2.2, 3.5],
                      "roughness": 0.2}}})

    say("Gameplay is a JSON file on disk — reviewable, diffable. (With a typo, on purpose.)")
    graph_path = os.path.join(root, "assets", "scripts", "demo_orb.json")
    os.makedirs(os.path.dirname(graph_path), exist_ok=True)
    json.dump(spin_graph("Logg"), open(graph_path, "w"), indent=1)
    print("  wrote assets/scripts/demo_orb.json (node type 'Logg' — oops)")
    cursor, _ = logs_since(0)
    rpc("component.set", {"name": "Orb", "component":
        {"type": "ScriptGraph", "graph": "assets/scripts/demo_orb.json"}})

    say("The engine doesn't fail silently — read the log like an agent would:")
    cursor, warns = logs_since(cursor, "unknown node type")
    for w in warns[:2]:
        print("  \033[93m%s\033[0m" % w)
    if not warns:
        raise SystemExit("expected a precise unknown-node warning")

    say("Fix the file, reattach — hot reload, no compile step.")
    json.dump(spin_graph("Log"), open(graph_path, "w"), indent=1)
    rpc("component.set", {"name": "Orb", "component":
        {"type": "ScriptGraph", "graph": "assets/scripts/demo_orb.json"}})
    cursor, _ = logs_since(cursor)

    say("Frame the shot and look at it — the agent has eyes.")
    rpc("camera.set", {"position": [3.2, 2.4, 4.2], "yaw": -125, "pitch": -12})
    shot = rpc("viewport.screenshot", {"path": "demo_build.bmp"})
    print("  screenshot: %s" % shot["path"])

    say("Aim the gameplay camera too — Play renders through IT, not the freecam.")
    rpc("entity.set", {"name": "MainCamera", "position": [0, 2.2, 5]})
    # CameraController convention: yaw 0 = -Z (NOT the freecam's -90).
    rpc("component.set", {"name": "MainCamera", "component":
        {"type": "CameraController", "yaw": 0, "pitch": -10}})

    say("Save the scene, PLAY it, and prove the script ran from its own logs.")
    rpc("scene.save", {"path": "assets/maps/demo.json"})
    rpc("play.start")
    time.sleep(max(PAUSE, 1.0))
    cursor, proof = logs_since(0, "demo: the orb is alive")
    print("  \033[92m%s\033[0m" % (proof[-1] if proof else "NO SCRIPT LOG — FAIL"))
    rpc("viewport.screenshot", {"path": "demo_play.bmp"})
    rpc("play.stop")
    if not proof:
        raise SystemExit("script log missing")

    say("Done: scene + script are plain JSON in the repo; the editor never needed a human.")
    print("  final check anyone can run:  AetherRuntime --project . --map "
          "assets/maps/demo.json --verify\n")

if __name__ == "__main__":
    main()
