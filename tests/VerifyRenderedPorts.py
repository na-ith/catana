#!/usr/bin/env python3
"""Verify that every rendered port is visibly linked to both edge vertices."""

from __future__ import annotations

import json
import sys
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: VerifyRenderedPorts.py SNAPSHOT.json RENDER.svg", file=sys.stderr)
        return 2

    snapshot = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    root = ET.parse(sys.argv[2]).getroot()
    edges = {edge["id"]: edge["vertices"] for edge in snapshot["topology"]["edges"]}

    expected = Counter(
        (port["edge"], vertex)
        for port in snapshot["board"]["ports"]
        for vertex in edges[port["edge"]]
    )
    actual = Counter(
        (int(element.attrib["data-port-edge"]), int(element.attrib["data-port-vertex"]))
        for element in root.iter("{http://www.w3.org/2000/svg}line")
        if element.attrib.get("class") == "port-link"
    )

    if actual != expected:
        print(f"expected port links {expected}, rendered {actual}", file=sys.stderr)
        return 1
    if sum(actual.values()) != 2 * len(snapshot["board"]["ports"]):
        print("each port must render exactly two links", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
