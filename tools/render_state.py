#!/usr/bin/env python3
"""Render an omniscient catan-debug-state-v1 JSON snapshot as an SVG image.

This module intentionally uses only Python's standard library. It is a debug
adapter, not part of the simulation or a stable save-game serializer.
"""

from __future__ import annotations

import argparse
import html
import http.server
import json
import math
import os
import socketserver
import sys
import threading
import time
import webbrowser
from pathlib import Path
from typing import Any


WIDTH = 1280
HEIGHT = 820
BOARD_LEFT = 35
BOARD_TOP = 55
BOARD_WIDTH = 790
BOARD_HEIGHT = 720
# SVG's y-axis points down, so a positive angle appears clockwise on screen.
BOARD_ROTATION_DEGREES = 60.0
PANEL_X = 850
ANALYSIS_X = 1275
ANALYSIS_WIDTH = 610
PLAYER_COLOURS = ("#d94a45", "#3978c5")
TERRAIN_COLOURS = {
    "forest": "#4d8651",
    "hills": "#b86546",
    "pasture": "#8fbd63",
    "fields": "#d9b951",
    "mountains": "#828990",
    "desert": "#d3bb88",
}
RESOURCE_COLOURS = {
    "lumber": "#397442",
    "brick": "#a94b36",
    "wool": "#78a951",
    "grain": "#c8992e",
    "ore": "#707982",
}


def esc(value: object) -> str:
    return html.escape(str(value), quote=True)


def text(
    x: float,
    y: float,
    value: object,
    css: str = "label",
    anchor: str = "start",
    extra: str = "",
) -> str:
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" class="{css}" {extra} '
        f'text-anchor="{anchor}">{esc(value)}</text>'
    )


def validate(data: dict[str, Any]) -> None:
    if data.get("schema") != "catan-debug-state-v1":
        raise ValueError("unsupported or missing debug snapshot schema")
    for key in ("turn", "topology", "board", "players", "bank", "awards"):
        if key not in data:
            raise ValueError(f"snapshot is missing {key!r}")
    if len(data["topology"].get("vertices", [])) != 54:
        raise ValueError("expected the standard 54-vertex topology")
    if len(data["topology"].get("edges", [])) != 72:
        raise ValueError("expected the standard 72-edge topology")
    if len(data["topology"].get("hexes", [])) != 19:
        raise ValueError("expected the standard 19-hex topology")


def write_viewer_command(command_file: Path, command: dict[str, Any]) -> None:
    """Validate and atomically publish one browser-to-stepper command."""
    command_type = command.get("type")
    position = int(command["position_version"])
    if position < 0:
        raise ValueError("invalid position version")
    if command_type == "action":
        action_index = int(command["action_index"])
        if action_index < 0:
            raise ValueError("invalid action index")
        payload = f"action {position} {action_index}\n"
    elif command_type == "random":
        payload = f"random {position}\n"
    elif command_type == "rewind":
        payload = f"rewind {position}\n"
    elif command_type == "tree":
        player = int(command["player"])
        enabled = bool(command["enabled"])
        if player not in (0, 1):
            raise ValueError("invalid tree player")
        payload = f"tree {position} {player} {int(enabled)}\n"
    else:
        raise ValueError("unknown command type")
    next_command = Path(str(command_file) + ".next")
    next_command.write_text(payload, encoding="utf-8")
    os.replace(next_command, command_file)


def coordinate_transform(vertices: list[dict[str, Any]]):
    unrotated = {
        v["id"]: (float(v["x"]), float(v["y"]) * math.sqrt(3.0))
        for v in vertices
    }
    centre_x = sum(point[0] for point in unrotated.values()) / len(unrotated)
    centre_y = sum(point[1] for point in unrotated.values()) / len(unrotated)
    angle = math.radians(BOARD_ROTATION_DEGREES)
    cosine = math.cos(angle)
    sine = math.sin(angle)
    raw = {}
    for vertex_id, (x_value, y_value) in unrotated.items():
        relative_x = x_value - centre_x
        relative_y = y_value - centre_y
        raw[vertex_id] = (
            centre_x + relative_x * cosine - relative_y * sine,
            centre_y + relative_x * sine + relative_y * cosine,
        )
    min_x = min(point[0] for point in raw.values())
    max_x = max(point[0] for point in raw.values())
    min_y = min(point[1] for point in raw.values())
    max_y = max(point[1] for point in raw.values())
    scale = min((BOARD_WIDTH - 85) / (max_x - min_x), (BOARD_HEIGHT - 75) / (max_y - min_y))
    x_offset = BOARD_LEFT + (BOARD_WIDTH - (max_x - min_x) * scale) / 2 - min_x * scale
    y_offset = BOARD_TOP + (BOARD_HEIGHT - (max_y - min_y) * scale) / 2 - min_y * scale

    def transform(vertex_id: int) -> tuple[float, float]:
        x_value, y_value = raw[vertex_id]
        return x_offset + x_value * scale, y_offset + y_value * scale

    return transform


def render(data: dict[str, Any]) -> str:
    validate(data)
    analysis = data.get("mcts")
    controls = data.get("controls") or {}
    position_version = int(controls.get("position_version", 0))
    can_rewind = bool(controls.get("can_rewind", False))
    tree_running = controls.get("tree_running", [True, True])
    analysis_rows = (
        len(analysis.get("actions", [])) + (1 if analysis.get("chance_node") else 0)
        if analysis else 0
    )
    canvas_width = 1900 if analysis else WIDTH
    canvas_height = max(HEIGHT, 165 + analysis_rows * 21) if analysis else HEIGHT
    topology = data["topology"]
    board = data["board"]
    vertices = {vertex["id"]: vertex for vertex in topology["vertices"]}
    edges = {edge["id"]: edge for edge in topology["edges"]}
    hex_topology = {tile["id"]: tile for tile in topology["hexes"]}
    board_hexes = {tile["id"]: tile for tile in board["hexes"]}
    point = coordinate_transform(list(vertices.values()))

    svg: list[str] = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{canvas_width}" height="{canvas_height}" '
        f'viewBox="0 0 {canvas_width} {canvas_height}" data-position-version="{position_version}" '
        f'data-can-rewind="{str(can_rewind).lower()}" '
        f'data-tree-running-p1="{str(bool(tree_running[0])).lower()}" '
        f'data-tree-running-p2="{str(bool(tree_running[1])).lower()}">',
        "<style>",
        "text { font-family: Inter, ui-sans-serif, system-ui, sans-serif; fill: #25313a; }",
        ".title { font-size: 24px; font-weight: 700; }",
        ".heading { font-size: 16px; font-weight: 700; }",
        ".label { font-size: 13px; }",
        ".small { font-size: 10px; }",
        ".tiny { font-size: 8px; }",
        ".token { font-size: 18px; font-weight: 800; }",
        ".white { fill: #fff; }",
        ".mcts-row { font-size: 11px; font-family: ui-monospace, SFMono-Regular, Menlo, monospace; }",
        ".mcts-action { cursor: pointer; }",
        ".chance-random-action { cursor: pointer; }",
        ".mcts-action.is-hovered rect { stroke: #e67827; stroke-width: 2; }",
        ".highlightable.is-highlighted { filter: drop-shadow(0 0 4px #f5a04b); }",
        "polygon.highlightable.is-highlighted { stroke: #f07824 !important; stroke-width: 7 !important; }",
        "circle.highlightable.is-highlighted { stroke: #f07824 !important; stroke-width: 4 !important; }",
        "line.highlightable.is-highlighted { stroke: #f07824 !important; stroke-width: 14 !important; }",
        "text.highlightable.is-highlighted, tspan.highlightable.is-highlighted { fill: #b94f16 !important; font-weight: 800; }",
        "g.highlightable.is-highlighted circle { stroke: #f07824 !important; stroke-width: 4 !important; }",
        "g.highlightable.is-highlighted text { fill: #b94f16 !important; font-weight: 800; }",
        "g.edge-highlight-target.is-highlighted line { stroke: #f07824; opacity: .95; stroke-width: 13; stroke-dasharray: 9 6; }",
        "</style>",
        f'<rect width="{canvas_width}" height="{canvas_height}" fill="#f3efe4"/>',
        f'<rect x="{BOARD_LEFT}" y="{BOARD_TOP}" width="{BOARD_WIDTH}" height="{BOARD_HEIGHT}" rx="24" fill="#83bad1"/>',
        f'<rect x="{PANEL_X}" y="28" width="400" height="764" rx="18" fill="#fffdf8" stroke="#d7d0c3" stroke-width="2"/>',
    ]

    all_points = [point(vertex_id) for vertex_id in vertices]
    board_center = (
        sum(value[0] for value in all_points) / len(all_points),
        sum(value[1] for value in all_points) / len(all_points),
    )

    # Ports sit outside their coastal edge and are drawn before the land tiles.
    for port in board["ports"]:
        ends = edges[port["edge"]]["vertices"]
        first, second = point(ends[0]), point(ends[1])
        midpoint = ((first[0] + second[0]) / 2, (first[1] + second[1]) / 2)
        direction = (midpoint[0] - board_center[0], midpoint[1] - board_center[1])
        length = math.hypot(*direction) or 1.0
        unit = (direction[0] / length, direction[1] / length)
        port_center = (midpoint[0] + unit[0] * 31, midpoint[1] + unit[1] * 31)
        for vertex_id, endpoint in zip(ends, (first, second)):
            svg.append(
                f'<line class="port-link" data-port-edge="{port["edge"]}" '
                f'data-port-vertex="{vertex_id}" x1="{endpoint[0]:.1f}" y1="{endpoint[1]:.1f}" '
                f'x2="{port_center[0]:.1f}" y2="{port_center[1]:.1f}" '
                'stroke="#735a3d" stroke-width="7" stroke-linecap="round"/>'
            )
        svg.append(
            f'<rect x="{port_center[0] - 25:.1f}" y="{port_center[1] - 12:.1f}" width="50" height="24" '
            'rx="7" fill="#f7e4b0" stroke="#735a3d" stroke-width="2"/>'
        )
        label = port["type"].replace("lumber", "L").replace("brick", "B").replace(
            "wool", "W"
        ).replace("grain", "G").replace("ore", "O")
        svg.append(text(port_center[0], port_center[1] + 4, label, "small", "middle"))

    for hex_id in sorted(hex_topology):
        tile_topology = hex_topology[hex_id]
        tile = board_hexes[hex_id]
        polygon = " ".join(f"{point(vertex)[0]:.1f},{point(vertex)[1]:.1f}" for vertex in tile_topology["vertices"])
        colour = TERRAIN_COLOURS.get(tile["terrain"], "#bbbbbb")
        svg.append(
            f'<polygon class="highlightable" data-highlight-id="hex-{hex_id}" '
            f'points="{polygon}" fill="{colour}" stroke="#f0dfb8" stroke-width="5"/>'
        )
        tile_points = [point(vertex) for vertex in tile_topology["vertices"]]
        center_x = sum(value[0] for value in tile_points) / 6
        center_y = sum(value[1] for value in tile_points) / 6
        svg.append(text(
            center_x, center_y - 34, f'H{hex_id} {tile["terrain"]}', "small", "middle",
            f'data-highlight-id="hex-{hex_id}"',
        ))

        # Draw the robber first so the number token sits in front of it.
        # Its smaller silhouette leaves the lower part visible beneath the token.
        if tile["robber"]:
            svg.append(f'<circle cx="{center_x:.1f}" cy="{center_y + 8:.1f}" r="7" fill="#2f3437" stroke="#fff" stroke-width="2"/>')
            svg.append(f'<rect x="{center_x - 9:.1f}" y="{center_y + 14:.1f}" width="18" height="23" rx="7" fill="#2f3437" stroke="#fff" stroke-width="2"/>')
            svg.append(text(center_x, center_y + 50, "ROBBER", "small", "middle"))

        if tile["number"]:
            svg.append(
                f'<circle class="highlightable" data-highlight-id="hex-token-{hex_id}" '
                f'cx="{center_x:.1f}" cy="{center_y:.1f}" r="22" fill="#f7edd1" stroke="#6b5b42"/>'
            )
            number_class = "token" if tile["number"] not in (6, 8) else "token"
            number_colour = "#b32626" if tile["number"] in (6, 8) else "#25313a"
            svg.append(
                f'<text x="{center_x:.1f}" y="{center_y + 6:.1f}" class="{number_class}" '
                f'text-anchor="middle" style="fill:{number_colour}">{tile["number"]}</text>'
            )
        else:
            # The desert has no number, but still gets a central hover target
            # for a robber action without reverting to a hex-edge highlight.
            svg.append(
                f'<circle class="highlightable" data-highlight-id="hex-token-{hex_id}" '
                f'cx="{center_x:.1f}" cy="{center_y:.1f}" r="22" fill="transparent" stroke="none"/>'
            )

    # Roads are above the terrain but below buildings.
    for edge_id, road in enumerate(board["roads"]):
        if road is None:
            continue
        ends = edges[edge_id]["vertices"]
        first, second = point(ends[0]), point(ends[1])
        colour = PLAYER_COLOURS[road["owner"]]
        svg.append(
            f'<line class="highlightable" data-highlight-id="edge-{edge_id}" '
            f'x1="{first[0]:.1f}" y1="{first[1]:.1f}" x2="{second[0]:.1f}" y2="{second[1]:.1f}" '
            'stroke="#392f2a" stroke-width="15" stroke-linecap="round"/>'
        )
        svg.append(
            f'<line class="highlightable" data-highlight-id="edge-{edge_id}" '
            f'x1="{first[0]:.1f}" y1="{first[1]:.1f}" x2="{second[0]:.1f}" y2="{second[1]:.1f}" '
            f'stroke="{colour}" stroke-width="10" stroke-linecap="round"/>'
        )
        midpoint = ((first[0] + second[0]) / 2, (first[1] + second[1]) / 2)
        svg.append(text(
            midpoint[0], midpoint[1] - 7, f'E{edge_id}', "tiny", "middle",
            f'data-highlight-id="edge-{edge_id}"',
        ))

    # Every edge gets a transparent target so an empty prospective road can
    # still be shown when its MCTS row is hovered.
    for edge_id, edge in edges.items():
        first, second = point(edge["vertices"][0]), point(edge["vertices"][1])
        svg.append(
            f'<g class="highlightable edge-highlight-target" data-highlight-id="edge-{edge_id}">'
            f'<line x1="{first[0]:.1f}" y1="{first[1]:.1f}" '
            f'x2="{second[0]:.1f}" y2="{second[1]:.1f}" '
            'stroke="transparent" stroke-width="22" stroke-linecap="round" pointer-events="none"/>'
            '</g>'
        )

    # Vertex identifiers remain visible even on empty intersections.
    for vertex_id in sorted(vertices):
        x_value, y_value = point(vertex_id)
        svg.append(f'<g class="highlightable" data-highlight-id="vertex-{vertex_id}">')
        svg.append(f'<circle cx="{x_value:.1f}" cy="{y_value:.1f}" r="7" fill="#f8f1dc" stroke="#695d4f"/>')
        svg.append(text(x_value, y_value + 3, vertex_id, "tiny", "middle"))
        svg.append('</g>')

    for vertex_id, building in enumerate(board["buildings"]):
        if building is None:
            continue
        x_value, y_value = point(vertex_id)
        colour = PLAYER_COLOURS[building["owner"]]
        if building["type"] == "city":
            shape = (
                f"{x_value - 20:.1f},{y_value + 15:.1f} {x_value - 20:.1f},{y_value - 8:.1f} "
                f"{x_value - 8:.1f},{y_value - 20:.1f} {x_value + 3:.1f},{y_value - 8:.1f} "
                f"{x_value + 21:.1f},{y_value - 8:.1f} {x_value + 21:.1f},{y_value + 15:.1f}"
            )
        else:
            shape = (
                f"{x_value - 15:.1f},{y_value + 13:.1f} {x_value - 15:.1f},{y_value - 5:.1f} "
                f"{x_value:.1f},{y_value - 18:.1f} {x_value + 15:.1f},{y_value - 5:.1f} "
                f"{x_value + 15:.1f},{y_value + 13:.1f}"
            )
        svg.append(f'<g class="highlightable" data-highlight-id="vertex-{vertex_id}">')
        svg.append(f'<polygon points="{shape}" fill="{colour}" stroke="#302925" stroke-width="3"/>')
        svg.append(text(x_value, y_value + 6, f'P{building["owner"] + 1}', "small white", "middle"))
        svg.append('</g>')

    turn = data["turn"]
    svg.append(text(PANEL_X + 25, 65, f'Turn {turn["number"]}', "title", extra='data-highlight-id="turn"'))
    svg.append(text(PANEL_X + 25, 91, turn["phase"], "heading", extra='data-highlight-id="turn"'))
    svg.append(text(
        PANEL_X + 25, 115, f'Active: P{turn["active_player"] + 1} · Decision: P{turn["decision_player"] + 1}',
        "label", extra='data-highlight-id="turn"',
    ))
    pending = f'Discard {turn.get("pending_discards", [])} · setup {turn.get("placement_index", 0)} · free roads {turn.get("free_roads_remaining", 0)} · plenty {turn.get("plenty_picks_remaining", 0)}'
    svg.append(text(PANEL_X + 25, 139, pending, "small"))
    if turn["winner"] is not None:
        svg.append(text(PANEL_X + 270, 91, f'Winner: P{turn["winner"] + 1}', "heading"))

    resource_order = ("lumber", "brick", "wool", "grain", "ore")
    card_order = ("knight", "road building", "year of plenty", "monopoly", "victory point")
    player_y = 165
    for player in data["players"]:
        colour = PLAYER_COLOURS[player["id"]]
        svg.append(f'<rect x="{PANEL_X + 20}" y="{player_y}" width="360" height="178" rx="13" fill="#f7f4ec" stroke="{colour}" stroke-width="4"/>')
        svg.append(f'<circle cx="{PANEL_X + 45}" cy="{player_y + 28}" r="13" fill="{colour}"/>')
        svg.append(text(PANEL_X + 68, player_y + 34, f'Player {player["id"] + 1}', "heading"))
        svg.append(text(PANEL_X + 245, player_y + 34, f'{player["total_vp"]} VP ({player["public_vp"]} public)', "heading"))
        x_value = PANEL_X + 34
        for resource in resource_order:
            resource_id = f'resource-{player["id"]}-{resource}'
            svg.append(f'<g class="highlightable" data-highlight-id="{resource_id}">')
            svg.append(f'<circle cx="{x_value + 14}" cy="{player_y + 67}" r="13" fill="{RESOURCE_COLOURS[resource]}"/>')
            svg.append(text(x_value + 14, player_y + 71, player["resources"][resource], "small white", "middle"))
            svg.append(text(x_value + 14, player_y + 91, resource[0].upper(), "small", "middle"))
            svg.append('</g>')
            x_value += 65
        svg.append(text(PANEL_X + 34, player_y + 119, f'Played knights: {player["played_knights"]}', "label"))
        card_labels = {"knight": "K", "road building": "RB", "year of plenty": "YOP", "monopoly": "M", "victory point": "VP"}
        svg.append(text(PANEL_X + 34, player_y + 143, "Development cards (omniscient):", "small"))
        card_x = PANEL_X + 34
        for card_name in card_order:
            card_label = (
                f'{card_labels[card_name]} {player["development_cards"][card_name]}'
                f'(+{player["bought_this_turn"][card_name]})'
            )
            card_id = f'devcard-{player["id"]}-{card_name.replace(" ", "-")}'
            svg.append(f'<g class="highlightable" data-highlight-id="{card_id}">')
            svg.append(text(card_x, player_y + 162, card_label, "small"))
            svg.append('</g>')
            card_x += max(48, len(card_label) * 5.7) + 5
        player_y += 194

    bank_y = 565
    svg.append(text(PANEL_X + 25, bank_y, "Bank", "heading"))
    bank_text = "   ".join(f'{name.title()}: {data["bank"][name]}' for name in resource_order)
    svg.append(text(PANEL_X + 25, bank_y + 23, bank_text, "small", extra='data-highlight-id="bank"'))
    deck_total = sum(data["development_deck"].values())
    svg.append(text(
        PANEL_X + 25, bank_y + 47, f'Development deck: {deck_total}', "small",
        extra='data-highlight-id="development-deck"',
    ))
    deck_x = PANEL_X + 155
    for card_name in card_order:
        card_id = f'development-deck-card-{card_name.replace(" ", "-")}'
        svg.append(f'<g class="highlightable" data-highlight-id="{card_id}">')
        svg.append(text(deck_x, bank_y + 47, f'{card_name[0].upper()} {data["development_deck"][card_name]}', "small"))
        svg.append('</g>')
        deck_x += 48

    awards = data["awards"]
    longest = "none" if awards["longest_road"] is None else f'P{awards["longest_road"] + 1}'
    army = "none" if awards["largest_army"] is None else f'P{awards["largest_army"] + 1}'
    svg.append(text(PANEL_X + 25, bank_y + 83, "Awards", "heading"))
    svg.append(text(PANEL_X + 25, bank_y + 107, f'Longest Road: {longest}  · lengths {awards["road_lengths"]}', "label"))
    svg.append(text(PANEL_X + 25, bank_y + 130, f'Largest Army: {army}', "label"))

    dice = data.get("dice", {})
    svg.append(text(PANEL_X + 25, bank_y + 166, "Balanced dice", "heading", extra='data-highlight-id="dice"'))
    streak_player = dice.get("seven_streak_player")
    streak = "none" if streak_player is None else f'P{streak_player + 1}×{dice.get("seven_streak_count", 0)}'
    svg.append(text(
        PANEL_X + 25, bank_y + 190,
        f'Deck: {dice.get("cards_left", "?")} · recent: {dice.get("recent_rolls", [])} · sevens: {dice.get("sevens_by_player", [])} · streak: {streak}',
        "label", extra='data-highlight-id="dice"',
    ))

    if analysis:
        panel_height = canvas_height - 56
        svg.append(
            f'<rect x="{ANALYSIS_X}" y="28" width="{ANALYSIS_WIDTH}" height="{panel_height}" '
            'rx="18" fill="#fffdf8" stroke="#d7d0c3" stroke-width="2"/>'
        )
        perspective = analysis.get("perspective", 0) + 1
        root_player = analysis.get("root_player", analysis.get("perspective", 0)) + 1
        svg.append(text(
            ANALYSIS_X + 22, 62,
            f'P{perspective} belief tree · P{root_player} decision',
            "title",
        ))
        node_kind = "chance outcomes" if analysis.get("chance_node") else "player decisions"
        svg.append(text(
            ANALYSIS_X + 22,
            86,
            f'{analysis.get("iterations", 0)} iterations · {analysis.get("workers", 1)} workers · '
            f'depth {analysis.get("maximum_depth", 0)}',
            "label",
        ))
        svg.append(text(
            ANALYSIS_X + 22,
            106,
            f'{analysis.get("unique_root_children", len(analysis.get("actions", [])))} unique nodes / '
            f'{len(analysis.get("actions", []))} actions · '
            f'{node_kind} · estimated win chance',
            "label",
        ))
        svg.append(text(ANALYSIS_X + 22, 127, "#    win       visits   action", "small"))
        ranked_actions = sorted(
            analysis.get("actions", []),
            key=lambda action: (
                -float(action.get("win_probability", 0.0)),
                -int(action.get("visits", 0)),
                int(action.get("index", 0)),
            ),
        )
        for row, action in enumerate(ranked_actions):
            y_value = 142 + row * 21
            recommended = bool(action.get("recommended"))
            row_class = "mcts-action recommended" if recommended else "mcts-action"
            fill = "#dff3df" if recommended else ("#f7f4ec" if row % 2 == 0 else "#fffdf8")
            stroke = "#4c9a57" if recommended else "none"
            targets = " ".join(str(target) for target in action.get("highlight_targets", []))
            svg.append(
                f'<g class="{row_class}" data-action-index="{action.get("index", row)}" '
                f'data-highlight-targets="{esc(targets)}">'
                f'<rect x="{ANALYSIS_X + 14}" y="{y_value - 13}" width="{ANALYSIS_WIDTH - 28}" '
                f'height="20" rx="4" fill="{fill}" stroke="{stroke}"/>'
            )
            win = float(action.get("win_probability", 0.0)) * 100.0
            chance = action.get("chance_probability")
            chance_text = "" if chance is None else f' · chance {float(chance) * 100.0:5.1f}%'
            marker = "★" if recommended else " "
            description = str(action.get("description", ""))
            if len(description) > 55:
                description = description[:52] + "..."
            label = (
                f'{marker}{int(action.get("index", row)):>3}  {win:6.2f}%  '
                f'{int(action.get("visits", 0)):>7}   {description}{chance_text}'
            )
            svg.append(text(ANALYSIS_X + 20, y_value + 2, label, "mcts-row"))
            svg.append("</g>")
        if analysis.get("chance_node"):
            y_value = 142 + len(ranked_actions) * 21
            svg.append(
                f'<g class="chance-random-action">'
                f'<rect x="{ANALYSIS_X + 14}" y="{y_value - 13}" width="{ANALYSIS_WIDTH - 28}" '
                'height="20" rx="4" fill="#dcecf8" stroke="#3978c5"/>'
            )
            svg.append(text(
                ANALYSIS_X + 20,
                y_value + 2,
                "     random weighted outcome",
                "mcts-row",
            ))
            svg.append("</g>")

    svg.append(text(42, 37, "Catan state debug view", "title"))
    svg.append(text(795, 804, "IDs are engine Hex/Vertex/Edge IDs · full hands are intentionally visible", "small", "end"))
    svg.append("</svg>")
    return "\n".join(svg)


def serve_live(
    snapshot: Path,
    ready_file: Path | None,
    command_file: Path | None,
    parent_pid: int | None,
    open_browser: bool,
    exit_after: float | None,
) -> int:
    """Serve a live SVG view of an atomically replaced state snapshot."""

    page = b"""<!doctype html>
<html><head><meta charset="utf-8"><title>Catan live state</title>
<style>
html,body { margin:0; min-height:100%; background:#25292d; font-family:system-ui,sans-serif; }
#bar { color:#e9ecef; padding:8px 14px; font-size:13px; display:flex; gap:18px; }
#status { color:#9fd49f; }
#rewind { border:1px solid #929da7; border-radius:5px; background:#3b454e; color:#fff; padding:3px 10px; cursor:pointer; }
#rewind:disabled { cursor:not-allowed; opacity:.45; }
.tree-toggle { display:flex; align-items:center; gap:5px; cursor:pointer; user-select:none; }
.tree-toggle input { accent-color:#6fb879; }
#state { display:block; width:min(1900px, calc(100vw - 20px)); height:auto; margin:0 auto 10px; box-shadow:0 3px 22px #111; }
#state svg { display:block; width:100%; height:auto; }
</style></head><body>
<div id="bar"><strong>Catan live state</strong><button id="rewind" disabled>&#8630; Rewind one action</button><label class="tree-toggle"><input id="tree-p1" type="checkbox" checked>P1 tree</label><label class="tree-toggle"><input id="tree-p2" type="checkbox" checked>P2 tree</label><span id="status">connected</span><span>Click an MCTS action to play it</span></div>
<div id="state" aria-label="Catan game state"></div>
<script>
let version = "";
const state = document.getElementById('state');
const rewind = document.getElementById('rewind');
const treeToggles = [document.getElementById('tree-p1'), document.getElementById('tree-p2')];
let hoveredActionIndex = null;
let positionVersion = 0;
let pendingPosition = null;

function actionRow(index) {
  if (!state.firstElementChild) return null;
  return Array.from(state.querySelectorAll('.mcts-action'))
    .find(row => row.dataset.actionIndex === String(index)) || null;
}

function clearHighlight() {
  state.querySelectorAll('.is-highlighted, .is-hovered').forEach(element => {
    element.classList.remove('is-highlighted', 'is-hovered');
  });
}

function highlightAction(row) {
  clearHighlight();
  if (!row) {
    hoveredActionIndex = null;
    return;
  }
  hoveredActionIndex = row.dataset.actionIndex;
  row.classList.add('is-hovered');
  const targets = (row.dataset.highlightTargets || '').split(/\\s+/).filter(Boolean);
  state.querySelectorAll('[data-highlight-id]').forEach(element => {
    if (targets.includes(element.dataset.highlightId)) element.classList.add('is-highlighted');
  });
}

state.addEventListener('mouseover', event => {
  const element = event.target instanceof Element ? event.target : event.target.parentElement;
  const row = element && element.closest('.mcts-action');
  const related = event.relatedTarget;
  if (row && (!(related instanceof Node) || !row.contains(related))) highlightAction(row);
});
state.addEventListener('mouseout', event => {
  const element = event.target instanceof Element ? event.target : event.target.parentElement;
  const row = element && element.closest('.mcts-action');
  const related = event.relatedTarget;
  if (row && (!(related instanceof Node) || !row.contains(related))) highlightAction(null);
});

async function sendCommand(type, actionIndex = null, extra = {}) {
  if (pendingPosition !== null) return;
  pendingPosition = positionVersion;
  rewind.disabled = true;
  document.getElementById('status').textContent = 'sending ' + type + '...';
  try {
    const response = await fetch('/command', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({type, position_version: positionVersion, action_index: actionIndex, ...extra}),
    });
    if (!response.ok) throw new Error(await response.text());
    if (type === 'tree') pendingPosition = null;
    else window.setTimeout(() => {
        if (pendingPosition === positionVersion) pendingPosition = null;
      }, 2000);
  } catch (error) {
    pendingPosition = null;
    document.getElementById('status').textContent = 'command failed: ' + error;
  }
}

state.addEventListener('click', event => {
  const element = event.target instanceof Element ? event.target : event.target.parentElement;
  const row = element && element.closest('.mcts-action');
  const randomRow = element && element.closest('.chance-random-action');
  if (row) sendCommand('action', Number(row.dataset.actionIndex));
  else if (randomRow) sendCommand('random');
});
rewind.addEventListener('click', () => sendCommand('rewind'));
treeToggles.forEach((toggle, player) => {
  toggle.addEventListener('change', () => sendCommand('tree', null, {player, enabled: toggle.checked}));
});

async function refresh() {
  try {
    const response = await fetch('/version', {cache:'no-store'});
    const next = await response.text();
    if (next !== version) {
      version = next;
      const image = await fetch('/image.svg?v=' + encodeURIComponent(next), {cache:'no-store'});
      state.innerHTML = await image.text();
      const svg = state.firstElementChild;
      const nextPosition = svg ? Number(svg.dataset.positionVersion || 0) : 0;
      if (pendingPosition !== null && nextPosition !== pendingPosition) pendingPosition = null;
      positionVersion = nextPosition;
      rewind.disabled = pendingPosition !== null || !svg || svg.dataset.canRewind !== 'true';
      if (svg) {
        treeToggles[0].checked = svg.dataset.treeRunningP1 !== 'false';
        treeToggles[1].checked = svg.dataset.treeRunningP2 !== 'false';
      }
      if (hoveredActionIndex !== null) highlightAction(actionRow(hoveredActionIndex));
    }
    document.getElementById('status').textContent = 'connected';
  } catch (_) {
    document.getElementById('status').textContent = 'viewer disconnected';
  }
}
setInterval(refresh, 150);
refresh();
</script></body></html>"""

    command_lock = threading.Lock()

    class LiveHandler(http.server.BaseHTTPRequestHandler):
        def send_bytes(self, payload: bytes, content_type: str, status: int = 200) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(payload)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            path = self.path.split("?", 1)[0]
            if path == "/":
                self.send_bytes(page, "text/html; charset=utf-8")
                return
            if path == "/version":
                try:
                    stat = snapshot.stat()
                    payload = f"{stat.st_mtime_ns}-{stat.st_size}".encode()
                    self.send_bytes(payload, "text/plain; charset=utf-8")
                except OSError as error:
                    self.send_bytes(str(error).encode(), "text/plain; charset=utf-8", 503)
                return
            if path == "/image.svg":
                try:
                    state = json.loads(snapshot.read_text(encoding="utf-8"))
                    payload = render(state).encode("utf-8")
                    self.send_bytes(payload, "image/svg+xml; charset=utf-8")
                except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
                    self.send_bytes(f"renderer error: {error}".encode(), "text/plain; charset=utf-8", 503)
                return
            self.send_bytes(b"not found", "text/plain; charset=utf-8", 404)

        def do_POST(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
            if self.path.split("?", 1)[0] != "/command" or command_file is None:
                self.send_bytes(b"not found", "text/plain; charset=utf-8", 404)
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if length <= 0 or length > 4096:
                    raise ValueError("invalid command size")
                command = json.loads(self.rfile.read(length).decode("utf-8"))
                with command_lock:
                    write_viewer_command(command_file, command)
                self.send_bytes(b'{"accepted":true}', "application/json")
            except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
                self.send_bytes(str(error).encode(), "text/plain; charset=utf-8", 400)

        def log_message(self, _format: str, *_arguments: object) -> None:
            return

    class LiveServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
        daemon_threads = True
        allow_reuse_address = True

    server = LiveServer(("127.0.0.1", 0), LiveHandler)
    url = f"http://127.0.0.1:{server.server_port}/"

    def monitor() -> None:
        started = time.monotonic()
        while True:
            time.sleep(0.2)
            if exit_after is not None and time.monotonic() - started >= exit_after:
                server.shutdown()
                return
            if parent_pid is not None:
                try:
                    os.kill(parent_pid, 0)
                except OSError:
                    server.shutdown()
                    return

    threading.Thread(target=monitor, daemon=True).start()
    if ready_file is not None:
        ready_file.parent.mkdir(parents=True, exist_ok=True)
        ready_file.write_text(url, encoding="utf-8")
    print(f"Live visualizer: {url}", flush=True)
    if open_browser:
        webbrowser.open_new(url)
    try:
        server.serve_forever(poll_interval=0.1)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", required=True, type=Path, help="catan-debug-state-v1 JSON snapshot")
    parser.add_argument("--output", type=Path, help="destination .svg image")
    parser.add_argument("--live", action="store_true", help="serve a live browser viewer")
    parser.add_argument("--ready-file", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--command-file", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--parent-pid", type=int, help=argparse.SUPPRESS)
    parser.add_argument("--no-browser", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--exit-after", type=float, help=argparse.SUPPRESS)
    arguments = parser.parse_args(argv)
    try:
        if arguments.live:
            return serve_live(
                arguments.input,
                arguments.ready_file,
                arguments.command_file,
                arguments.parent_pid,
                not arguments.no_browser,
                arguments.exit_after,
            )
        if arguments.output is None:
            raise ValueError("--output is required unless --live is used")
        if arguments.output.suffix.lower() != ".svg":
            raise ValueError("output path must end in .svg")
        data = json.loads(arguments.input.read_text(encoding="utf-8"))
        image = render(data)
        arguments.output.parent.mkdir(parents=True, exist_ok=True)
        arguments.output.write_text(image, encoding="utf-8")
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        print(f"render_state.py: {error}", file=sys.stderr)
        return 2
    print(arguments.output.resolve())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
