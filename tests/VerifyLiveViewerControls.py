#!/usr/bin/env python3
"""Verify live-view metadata and browser command publication without sockets."""

from __future__ import annotations

import importlib.util
import json
import math
import sys
import tempfile
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: VerifyLiveViewerControls.py RENDERER SNAPSHOT", file=sys.stderr)
        return 2

    renderer_path = Path(sys.argv[1])
    specification = importlib.util.spec_from_file_location("catan_render_state", renderer_path)
    if specification is None or specification.loader is None:
        print("could not load renderer", file=sys.stderr)
        return 1
    renderer = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(renderer)

    snapshot = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    if not math.isclose(renderer.BOARD_ROTATION_DEGREES, -30.0):
        print("visualizer board is not rotated anticlockwise by 30 degrees", file=sys.stderr)
        return 1
    vertices = {vertex["id"]: vertex for vertex in snapshot["topology"]["vertices"]}
    first_edge = snapshot["topology"]["edges"][0]["vertices"]
    start, end = (vertices[vertex_id] for vertex_id in first_edge)
    raw_dx = float(end["x"]) - float(start["x"])
    raw_dy = (float(end["y"]) - float(start["y"])) * math.sqrt(3.0)
    rotation = math.radians(renderer.BOARD_ROTATION_DEGREES)
    expected_dx = raw_dx * math.cos(rotation) - raw_dy * math.sin(rotation)
    expected_dy = raw_dx * math.sin(rotation) + raw_dy * math.cos(rotation)
    transform = renderer.coordinate_transform(list(vertices.values()))
    transformed_start = transform(start["id"])
    transformed_end = transform(end["id"])
    actual_dx = transformed_end[0] - transformed_start[0]
    actual_dy = transformed_end[1] - transformed_start[1]
    cross_product = actual_dx * expected_dy - actual_dy * expected_dx
    dot_product = actual_dx * expected_dx + actual_dy * expected_dy
    tolerance = 1e-9 * math.hypot(actual_dx, actual_dy) * math.hypot(expected_dx, expected_dy)
    if abs(cross_product) > tolerance or dot_product <= 0.0:
        print("visualizer coordinate transform does not apply its board rotation", file=sys.stderr)
        return 1
    snapshot["controls"] = {
        "position_version": 17,
        "can_rewind": True,
        "tree_running": [True, False],
    }
    image = renderer.render(snapshot)
    if 'data-position-version="17"' not in image or 'data-can-rewind="true"' not in image:
        print("SVG does not expose live control metadata", file=sys.stderr)
        return 1
    if ('data-tree-running-p1="true"' not in image or
            'data-tree-running-p2="false"' not in image):
        print("SVG does not expose player-tree running states", file=sys.stderr)
        return 1
    chance_snapshot = dict(snapshot)
    chance_snapshot["mcts"] = dict(snapshot["mcts"])
    chance_snapshot["mcts"]["chance_node"] = True
    if 'class="chance-random-action"' not in renderer.render(chance_snapshot):
        print("chance analysis does not render a random option", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory(prefix="catan-viewer-test-") as directory:
        command_file = Path(directory) / "command"
        renderer.write_viewer_command(
            command_file,
            {"type": "action", "position_version": 7, "action_index": 3},
        )
        if command_file.read_text(encoding="utf-8") != "action 7 3\n":
            print("action click was not published correctly", file=sys.stderr)
            return 1
        renderer.write_viewer_command(
            command_file,
            {"type": "random", "position_version": 7, "action_index": None},
        )
        if command_file.read_text(encoding="utf-8") != "random 7\n":
            print("random click was not published correctly", file=sys.stderr)
            return 1
        renderer.write_viewer_command(
            command_file,
            {"type": "rewind", "position_version": 8, "action_index": None},
        )
        if command_file.read_text(encoding="utf-8") != "rewind 8\n":
            print("rewind click was not published correctly", file=sys.stderr)
            return 1
        renderer.write_viewer_command(
            command_file,
            {"type": "tree", "position_version": 9, "player": 1, "enabled": False},
        )
        if command_file.read_text(encoding="utf-8") != "tree 9 1 0\n":
            print("tree toggle was not published correctly", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
