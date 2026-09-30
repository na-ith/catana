#!/usr/bin/env python3
"""Verify the stepper's synthetic weighted-random chance option."""

from __future__ import annotations

import subprocess
import sys


def run_stepper(executable: str, selection: str) -> str:
    # Repeatedly selecting action zero completes the four setup placements and
    # starts P1's first roll. The standard balanced deck then has 11 outcomes,
    # making synthetic option 11 the weighted-random selection.
    commands = "0\n" * 9 + selection + "\nq\n"
    result = subprocess.run(
        [executable, "--seed", "42"],
        input=commands,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0:
        raise RuntimeError(f"stepper exited with {result.returncode}\n{result.stdout}")
    return result.stdout


def verify(output: str, selection: str) -> None:
    if "11: random  [sample a weighted chance outcome]" not in output:
        raise RuntimeError(f"random option was not listed for {selection!r}")
    if "Randomly selected " not in output or "[p=" not in output:
        raise RuntimeError(f"random outcome was not reported for {selection!r}")
    if "Invalid selection." in output:
        raise RuntimeError(f"random option was rejected for {selection!r}")


def verify_rewind(executable: str) -> None:
    result = subprocess.run(
        [executable, "--seed", "42"],
        input="0\nrewind\nq\n",
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    if result.returncode != 0 or "Rewound one action." not in result.stdout:
        raise RuntimeError("stepper rewind command failed")
    if result.stdout.count("Turn 0 | initial settlement") < 2:
        raise RuntimeError("rewind did not restore the preceding complete state")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: VerifyStepperRandom.py CATAN_CLI", file=sys.stderr)
        return 2
    try:
        verify(run_stepper(sys.argv[1], "random"), "random")
        verify(run_stepper(sys.argv[1], "11"), "11")
        verify_rewind(sys.argv[1])
    except RuntimeError as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
