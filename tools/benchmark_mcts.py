#!/usr/bin/env python3
"""Build and run repeatable MCTS performance benchmarks across game states."""

from __future__ import annotations

import argparse
import csv
import io
import os
import platform
import statistics
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCENARIOS = (
    "initial_settlement",
    "initial_road",
    "dice_chance",
    "early_main",
    "midgame_main",
    "trade_rich_main",
    "robber_decision",
    "simultaneous_discard",
    "hidden_card_information",
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run optimized, repeatable MCTS benchmarks for representative game positions."
    )
    parser.add_argument("--scenario", choices=("all", *SCENARIOS), default="all")
    parser.add_argument("--iterations", type=int, default=2000,
                        help="MCTS iterations per measured search (default: 2000)")
    parser.add_argument("--repeats", type=int, default=3,
                        help="independent seeded measurements per scenario (default: 3)")
    parser.add_argument("--rollout-steps", type=int, default=500,
                        help="maximum rollout length (default: 500, matching MctsConfig)")
    parser.add_argument("--trade-batch-states", type=int, default=512,
                        help="trade enumeration budget per node (default: 512)")
    parser.add_argument("--seed", type=int, default=42,
                        help="board and base search seed (default: 42)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 1,
                        help="parallel build jobs (default: detected CPU count)")
    parser.add_argument("--build-dir", type=Path, default=ROOT / "build" / "mcts-benchmark",
                        help="CMake build directory")
    parser.add_argument("--no-build", action="store_true",
                        help="use an already-built benchmark executable")
    parser.add_argument("--csv", type=Path,
                        help="write per-run measurements to this CSV file")
    args = parser.parse_args()
    for name in ("iterations", "repeats", "rollout_steps", "trade_batch_states", "jobs"):
        if getattr(args, name) <= 0:
            parser.error(f"--{name.replace('_', '-')} must be positive")
    if not 0 <= args.seed <= 0xFFFFFFFF:
        parser.error("--seed must be between 0 and 4294967295")
    return args


def build(args: argparse.Namespace) -> Path:
    build_dir = args.build_dir.expanduser().resolve()
    executable = build_dir / "catan_mcts_benchmark"
    if not args.no_build:
        subprocess.run(
            ["cmake", "-S", str(ROOT), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release"],
            check=True,
        )
        subprocess.run(
            ["cmake", "--build", str(build_dir), "--target", "catan_mcts_benchmark",
             "--parallel", str(args.jobs)],
            check=True,
        )
    if not executable.is_file():
        raise SystemExit(f"benchmark executable not found: {executable}; run without --no-build")
    return executable


def run_benchmarks(executable: Path, args: argparse.Namespace) -> list[dict[str, str]]:
    command = [
        str(executable),
        "--scenario", args.scenario,
        "--iterations", str(args.iterations),
        "--repeats", str(args.repeats),
        "--rollout-steps", str(args.rollout_steps),
        "--trade-batch-states", str(args.trade_batch_states),
        "--seed", str(args.seed),
    ]
    completed = subprocess.run(command, check=True, capture_output=True, text=True)
    rows = list(csv.DictReader(io.StringIO(completed.stdout)))
    if not rows:
        raise SystemExit("benchmark produced no measurements")
    for row in rows:
        row["seed"] = str(args.seed + int(row["repeat"]) - 1)
        row["rollout_steps"] = str(args.rollout_steps)
        row["trade_batch_states"] = str(args.trade_batch_states)
    return rows


def print_summary(rows: list[dict[str, str]], args: argparse.Namespace) -> None:
    print(f"Release MCTS benchmark: {args.iterations} iterations, "
          f"{args.rollout_steps} rollout steps, {args.repeats} repeats, base seed {args.seed}")
    print(f"Host: {platform.system()} {platform.release()} ({platform.machine()})")
    print()
    print(f"{'scenario':24} {'root actions':>12} {'worlds':>8} {'median seconds':>15} "
          f"{'median iter/s':>15} {'median depth':>13}")
    for scenario in dict.fromkeys(row["scenario"] for row in rows):
        group = [row for row in rows if row["scenario"] == scenario]
        elapsed = statistics.median(float(row["elapsed_seconds"]) for row in group)
        rate = statistics.median(float(row["iterations_per_second"]) for row in group)
        depth = statistics.median(int(row["max_depth"]) for row in group)
        actions = statistics.median(int(row["root_actions"]) for row in group)
        worlds = statistics.median(int(row["worlds"]) for row in group)
        print(f"{scenario:24} {actions:12.0f} {worlds:8.0f} {elapsed:15.4f} "
              f"{rate:15,.0f} {depth:13.0f}")


def write_csv(path: Path, rows: list[dict[str, str]]) -> None:
    path = path.expanduser().resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    columns = [
        "scenario", "repeat", "seed", "iterations", "rollout_steps", "trade_batch_states",
        "elapsed_seconds", "iterations_per_second", "root_actions", "max_depth", "worlds",
    ]
    with path.open("w", newline="", encoding="utf-8") as output:
        writer = csv.DictWriter(output, fieldnames=columns)
        writer.writeheader()
        writer.writerows(rows)
    print(f"Raw measurements: {path}")


def main() -> int:
    args = parse_args()
    executable = build(args)
    rows = run_benchmarks(executable, args)
    print_summary(rows, args)
    if args.csv:
        write_csv(args.csv, rows)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except subprocess.CalledProcessError as error:
        print(f"command failed with exit status {error.returncode}: {error.cmd}", file=sys.stderr)
        if error.stderr:
            print(error.stderr, file=sys.stderr, end="" if error.stderr.endswith("\n") else "\n")
        raise SystemExit(error.returncode)
