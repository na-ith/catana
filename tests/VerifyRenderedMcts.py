#!/usr/bin/env python3
"""Verify that the debug SVG contains every MCTS root action and recommendation."""

from __future__ import annotations

import json
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: VerifyRenderedMcts.py SNAPSHOT.json RENDER.svg", file=sys.stderr)
        return 2

    snapshot = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    analysis = snapshot.get("mcts")
    if not analysis:
        print("snapshot has no MCTS analysis", file=sys.stderr)
        return 1
    if "maximum_depth" not in analysis or analysis["maximum_depth"] < 1:
        print("snapshot has no explored MCTS depth", file=sys.stderr)
        return 1
    if analysis.get("workers", 0) < 1:
        print("snapshot has no MCTS worker count", file=sys.stderr)
        return 1
    unique_children = analysis.get("unique_root_children", 0)
    if unique_children < 1 or unique_children > len(analysis.get("actions", [])):
        print("snapshot has an invalid unique MCTS root-child count", file=sys.stderr)
        return 1

    root = ET.parse(sys.argv[2]).getroot()
    rows = [element for element in root.iter("{http://www.w3.org/2000/svg}g")
            if "mcts-action" in element.attrib.get("class", "").split()]
    highlight_ids = {
        element.attrib["data-highlight-id"]
        for element in root.iter()
        if "data-highlight-id" in element.attrib
    }
    actual_indices = {int(row.attrib["data-action-index"]) for row in rows}
    expected_indices = {int(action["index"]) for action in analysis["actions"]}
    if actual_indices != expected_indices or len(rows) != len(analysis["actions"]):
        print(f"expected MCTS rows {expected_indices}, rendered {actual_indices}", file=sys.stderr)
        return 1

    expected_order = [
        int(action["index"])
        for action in sorted(
            analysis["actions"],
            key=lambda action: (
                -float(action["win_probability"]),
                -int(action["visits"]),
                int(action["index"]),
            ),
        )
    ]
    actual_order = [int(row.attrib["data-action-index"]) for row in rows]
    if actual_order != expected_order:
        print(f"expected MCTS order {expected_order}, rendered {actual_order}", file=sys.stderr)
        return 1

    for action, row in zip(
        sorted(
            analysis["actions"],
            key=lambda action: (
                -float(action["win_probability"]),
                -int(action["visits"]),
                int(action["index"]),
            ),
        ),
        rows,
    ):
        expected_targets = set(action.get("highlight_targets", []))
        rendered_targets = set(row.attrib.get("data-highlight-targets", "").split())
        if not expected_targets or not expected_targets.issubset(highlight_ids):
            print(f"action {action['index']} has missing debug highlight targets {expected_targets}", file=sys.stderr)
            return 1
        if rendered_targets != expected_targets:
            print(
                f"action {action['index']} expected row targets {expected_targets}, rendered {rendered_targets}",
                file=sys.stderr,
            )
            return 1

    recommended = [row for row in rows if "recommended" in row.attrib.get("class", "").split()]
    best = analysis.get("best_action_index")
    if best is None:
        if recommended:
            print("chance analysis unexpectedly highlights a recommendation", file=sys.stderr)
            return 1
    elif len(recommended) != 1 or int(recommended[0].attrib["data-action-index"]) != best:
        print(f"expected recommendation {best}, rendered {recommended}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
