#!/usr/bin/env python3
"""Exercise captured-message import and CLI initialization end to end."""

import copy
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run(cli, args, commands, expected_return=0):
    result = subprocess.run([cli, *args], input=commands, text=True,
                            capture_output=True, timeout=15)
    assert result.returncode == expected_return, (result.returncode, result.stdout, result.stderr)
    return result.stdout


def main():
    cli, snapshot_path = sys.argv[1:]
    snapshot = json.loads(Path(snapshot_path).read_text())
    tiles = snapshot["board"]["hexes"]
    terrain_enums = {"desert": 0, "forest": 1, "hills": 2, "pasture": 3,
                     "fields": 4, "mountains": 5}
    # Colonist's port enum differs from its resource-card enum.
    port_enums = {"3:1": 1, "lumber 2:1": 2, "brick 2:1": 3,
                  "wool 2:1": 4, "grain 2:1": 5, "ore 2:1": 6}
    topo = snapshot["topology"]
    edges = {tuple(sorted(edge["vertices"])): edge["id"] for edge in topo["edges"]}
    # Build source faces from C++'s exported coordinates, with unrelated source
    # IDs and a translation, rather than relying on Python's generated topology.
    faces = {}
    for tile in topo["hexes"]:
        q = tile["x"] // 3
        r = (tile["y"] - q) // 2
        faces[tile["id"]] = q + 7, r - 4
    source_tiles = {}
    for tile in reversed(tiles):
        x, y = faces[tile["id"]]
        source_tiles[str(900 - tile["id"] * 11)] = {
            "x": x, "y": y, "type": terrain_enums[tile["terrain"]],
            "diceNumber": tile["number"],
        }
    source_tiles["999"] = {"x": 100, "y": 100, "type": 8, "diceNumber": 0}
    source_ports = {}
    for number, port in enumerate(snapshot["board"]["ports"]):
        for tile in topo["hexes"]:
            vertices = tile["vertices"]
            found = False
            for side in range(6):
                edge = edges[tuple(sorted((vertices[side], vertices[(side + 1) % 6])))]
                if edge != port["edge"]:
                    continue
                x, y = faces[tile["id"]]
                # Independent inverse of the source hex-edge definitions.
                if side == 0: x, z = x + 1, 1
                elif side == 1: y, z = y + 1, 0
                elif side == 2: z = 2
                elif side == 3: z = 1
                elif side == 4: z = 0
                else: x, y, z = x + 1, y - 1, 2
                source_ports[str(80 + number * 3)] = {
                    "x": x, "y": y, "z": z, "type": port_enums[port["type"]],
                }
                found = True
                break
            if found: break
    captured = {"direction": "received", "decoded": {"format": "messagepack", "value": {
        "id": 123, "data": {"gameState": {"mapState": {
            "tileHexStates": source_tiles, "portEdgeStates": source_ports,
        }}}}}}
    with tempfile.TemporaryDirectory(prefix="catan-board-import-") as directory:
        directory = Path(directory)
        capture = directory / "capture with spaces.jsonl"
        capture.write_text(json.dumps({"direction": "received", "decoded": {"value": {
            "id": 91, "data": {"diff": {"mapState": {"tileHexStates": {}, "portEdgeStates": {}}}}
        }}}) + "\n" + json.dumps(captured) + "\n")
        exported = directory / "actual.json"
        output = run(cli, ["--uninitialized"],
                     f"status\n0\ninit {directory / 'missing.json'}\nstatus\n"
                     f"init {capture}\njson {exported}\n0\nrewind\nq\n")
        assert output.count("State: uninitialized") == 2, output
        assert "Initialize first" in output and "Board initialization failed" in output
        assert "Board initialized from" in output and "Rewound one action" in output
        actual = json.loads(exported.read_text())
        assert actual["board"] == snapshot["board"], (actual["board"], snapshot["board"])
        assert actual["turn"]["phase"] == "initial settlement"
        assert actual["dice"] == snapshot["dice"]
        assert actual["bank"] == snapshot["bank"]

        # Starting directly from a capture follows exactly the same path.
        assert "imported board" in run(cli, ["--board", str(capture)], "q\n")
        output = run(cli, ["--uninitialized"],
                     "init-json {broken\nstatus\ninit-json " + json.dumps(captured) + "\nq\n")
        assert "State: uninitialized" in output and "Board initialized from" in output
        run(cli, ["--seed", "4", "--uninitialized"], "", 2)

        # A complete but corrupt board must fail atomically and allow retry.
        bad = copy.deepcopy(captured)
        board_map = bad["decoded"]["value"]["data"]["gameState"]["mapState"]
        port_keys = list(board_map["portEdgeStates"])
        board_map["portEdgeStates"][port_keys[1]] = board_map["portEdgeStates"][port_keys[0]]
        invalid = directory / "invalid.json"
        invalid.write_text(json.dumps(bad))
        output = run(cli, ["--uninitialized"], f"init {invalid}\nstatus\ninit {capture}\nq\n")
        assert "ports overlap" in output and "State: uninitialized" in output, output

        # Canonical board API format needs no webpage field names.
        normalized = {
            "schema": "catan-board-v1",
            "hexes": [{key: tile[key] for key in ("id", "terrain", "number")} for tile in tiles],
            "ports": [{"edge": port["edge"], "type": "generic" if port["type"] == "3:1"
                       else port["type"].split()[0]} for port in snapshot["board"]["ports"]],
            "robber_hex": next(tile["id"] for tile in tiles if tile["robber"]),
        }
        canonical = directory / "canonical.json"
        canonical.write_text(json.dumps(normalized))
        run(cli, ["--board", str(canonical)], "q\n")
        normalized["hexes"][0]["id"] = normalized["hexes"][1]["id"]
        canonical.write_text(json.dumps(normalized))
        run(cli, ["--board", str(canonical)], "", 2)
    print("Board import and uninitialized CLI checks passed")


if __name__ == "__main__":
    main()
