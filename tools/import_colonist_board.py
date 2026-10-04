#!/usr/bin/env python3
"""Translate a decoded Colonist snapshot/capture into an initial engine board.

The CLI adapter consumes the text format emitted by --engine-text. The default
output is the documented catan-board-v1 JSON format, useful for other clients.
Only layout is imported: a new game always starts before initial placement.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

TERRAINS = ("forest", "hills", "pasture", "fields", "mountains", "desert")
PORTS = ("generic", "lumber", "brick", "wool", "grain", "ore")
COLONIST_TERRAINS = {0: "desert", 1: "forest", 2: "hills", 3: "pasture",
                     4: "fields", 5: "mountains"}
CORNER_OFFSETS = ((2, 0), (1, 1), (-1, 1), (-2, 0), (-1, -1), (1, -1))


def standard_topology():
    """Same axial coordinates and insertion order as BoardTopology::standard()."""
    centres = [(q, r) for r in range(-2, 3)
               for q in range(max(-2, -r - 2), min(2, -r + 2) + 1)]
    vertex_ids = {}
    edge_ids = {}
    sides = {}
    for hex_id, (q, r) in enumerate(centres):
        cx, cy = 3 * q, 2 * r + q
        vertices = []
        for dx, dy in CORNER_OFFSETS:
            position = cx + dx, cy + dy
            vertex_ids.setdefault(position, len(vertex_ids))
            vertices.append(vertex_ids[position])
        for side in range(6):
            pair = tuple(sorted((vertices[side], vertices[(side + 1) % 6])))
            edge_ids.setdefault(pair, len(edge_ids))
            sides[hex_id, side] = edge_ids[pair]
    return centres, sides


def integer(value, label):
    if type(value) is not int:
        raise ValueError(f"{label} must be an integer")
    return value


def entries(values, label):
    if isinstance(values, dict):
        return [(str(key), value) for key, value in values.items() if value is not None]
    if isinstance(values, list):
        return [(str(key), value) for key, value in enumerate(values) if value is not None]
    raise ValueError(f"{label} must be an indexed object or array")


def port_name(value):
    # Normalized engine port values; Colonist's wire enum is separate.
    if type(value) is int and value in (0, 9):
        return "generic"
    if type(value) is int and 1 <= value <= 5:
        return PORTS[value]
    if isinstance(value, str) and value.lower() in PORTS:
        return value.lower()
    raise ValueError(f"unsupported port type {value!r}; expected generic or resource 1..5")


def colonist_port_name(value):
    # Colonist shared-async module 47474: Port=1, PortLumber=2,
    # PortBrick=3, PortWool=4, PortGrain=5, PortOre=6.
    if type(value) is int and 1 <= value <= 6:
        return PORTS[value - 1]
    if isinstance(value, str):
        return port_name(value)
    raise ValueError(f"unsupported Colonist port type {value!r}; expected 1..6")


def translate_map(map_state):
    centres, sides = standard_topology()
    axial_ids = {centre: hex_id for hex_id, centre in enumerate(centres)}
    land = []
    for source_id, tile in entries(map_state["tileHexStates"], "tileHexStates"):
        if not isinstance(tile, dict):
            raise ValueError("tile entry must be an object")
        tile_type = integer(tile.get("type"), "tile type")
        if tile_type == 8:  # sea is not part of the 19-hex engine board
            continue
        if tile_type not in COLONIST_TERRAINS:
            raise ValueError(f"unsupported land tile type {tile_type}")
        land.append((source_id, tile))
    if len(land) != 19:
        raise ValueError(f"complete standard board needs 19 land tiles; found {len(land)}")
    xs = [integer(tile.get("x"), "tile x") for _, tile in land]
    ys = [integer(tile.get("y"), "tile y") for _, tile in land]
    if sum(xs) % 19 or sum(ys) % 19:
        raise ValueError("land coordinates do not form a standard radius-two hex board")
    origin_x, origin_y = sum(xs) // 19, sum(ys) // 19
    hexes = []
    seen = set()
    mapping = {}
    for source_id, tile in land:
        coordinate = tile["x"] - origin_x, tile["y"] - origin_y
        hex_id = axial_ids.get(coordinate)
        if hex_id is None or hex_id in seen:
            raise ValueError("duplicate or unsupported land coordinates")
        seen.add(hex_id)
        mapping[source_id] = hex_id
        hexes.append({"id": hex_id, "terrain": COLONIST_TERRAINS[tile["type"]],
                      "number": integer(tile.get("diceNumber"), "diceNumber")})

    ports = []
    # Colonist edge z=0/1/2 is NW/W/SW. Each names two axial faces;
    # locate the land face and its corresponding engine side.
    neighbours = ((0, -1, 4), (-1, 0, 3), (-1, 1, 2))
    port_mapping = {}
    for source_id, port in entries(map_state["portEdgeStates"], "portEdgeStates"):
        if not isinstance(port, dict):
            raise ValueError("port entry must be an object")
        x = integer(port.get("x"), "port x") - origin_x
        y = integer(port.get("y"), "port y") - origin_y
        z = integer(port.get("z"), "port z")
        if not 0 <= z <= 2:
            raise ValueError("port z must be 0 (NW), 1 (W), or 2 (SW)")
        dx, dy, side = neighbours[z]
        first, second = axial_ids.get((x, y)), axial_ids.get((x + dx, y + dy))
        if (first is None) == (second is None):
            raise ValueError("port must connect exactly one land face and one sea face")
        edge = sides[first, side] if first is not None else sides[second, (side + 3) % 6]
        ports.append({"edge": edge, "type": colonist_port_name(port.get("type"))})
        port_mapping[source_id] = edge
    if len(ports) != 9:
        raise ValueError(f"complete standard board needs 9 ports; found {len(ports)}")
    desert = [tile["id"] for tile in hexes if tile["terrain"] == "desert"]
    if len(desert) != 1:
        raise ValueError("board must contain exactly one desert")
    return {"schema": "catan-board-v1", "hexes": sorted(hexes, key=lambda tile: tile["id"]),
            "ports": ports, "robber_hex": desert[0],
            "source_mapping": {"hexes": mapping, "ports": port_mapping}}


def candidates(value):
    if isinstance(value, dict):
        if value.get("schema") == "catan-board-v1":
            yield value
            return
        if "tileHexStates" in value and "portEdgeStates" in value:
            yield value
            return
        # The capture includes outgoing envelopes as well as incoming states.
        if value.get("direction") == "sent":
            return
        for child in value.values():
            yield from candidates(child)
    elif isinstance(value, list):
        for child in value:
            yield from candidates(child)


def read_board(path):
    content = Path(path).read_text(encoding="utf-8")
    try:
        records = [json.loads(content)]
    except json.JSONDecodeError:
        records = [json.loads(line) for line in content.splitlines() if line.strip()]
    errors = []
    for record in records:
        for candidate in candidates(record):
            try:
                return candidate if candidate.get("schema") == "catan-board-v1" else translate_map(candidate)
            except (ValueError, KeyError) as error:
                errors.append(str(error))
    detail = errors[0] if errors else "no mapState with tileHexStates and portEdgeStates found"
    raise ValueError(f"no complete supported board snapshot: {detail}")


def engine_text(board):
    hexes = board.get("hexes")
    ports = board.get("ports")
    if not isinstance(hexes, list) or len(hexes) != 19:
        raise ValueError("hexes must contain exactly 19 entries")
    if not isinstance(ports, list) or len(ports) != 9:
        raise ValueError("ports must contain exactly 9 entries")
    lines = ["CATAN_BOARD_V1"]
    for tile in hexes:
        terrain = tile.get("terrain")
        if terrain not in TERRAINS:
            raise ValueError(f"unsupported terrain {terrain!r}")
        lines.append(f"hex {integer(tile.get('id'), 'hex id')} {TERRAINS.index(terrain)} "
                     f"{integer(tile.get('number'), 'number')}")
    for port in ports:
        name = port_name(port.get("type"))
        lines.append(f"port {integer(port.get('edge'), 'port edge')} {PORTS.index(name)}")
    lines.append(f"robber {integer(board.get('robber_hex'), 'robber_hex')}")
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="decoded snapshot JSON or capture JSONL")
    parser.add_argument("--engine-text", action="store_true")
    args = parser.parse_args()
    try:
        board = read_board(args.input)
        # Validate basic schema even when the caller wants normalized JSON.
        text = engine_text(board)
        print(text if args.engine_text else json.dumps(board, indent=2))
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"ERROR {error}")
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
