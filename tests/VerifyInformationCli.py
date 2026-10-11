"""Exercise both observer CLI modes through real setup, buys, and rewind."""
import json
import os
import re
import select
import subprocess
import sys
import tempfile
import time
import urllib.request
import urllib.error
from pathlib import Path


class Stepper:
    def __init__(self, cli, observer, live=False):
        arguments = [cli, "42"]
        if observer is not None:
            arguments += ["--perspective", observer]
        if live:
            arguments += ["--visualizer", "--mcts-threads", "1"]
        self.process = subprocess.Popen(arguments,
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, bufsize=0,
                                        env={**os.environ, "BROWSER": "/usr/bin/true"})
        self.deadline = time.monotonic() + 40
        self.output = self.read()

    def read(self):
        data = b""
        while not data.endswith(b"> "):
            assert time.monotonic() < self.deadline, data.decode()
            ready, _, _ = select.select([self.process.stdout], [], [], 1)
            if not ready:
                continue
            chunk = os.read(self.process.stdout.fileno(), 65536)
            assert chunk, (self.process.poll(), data.decode())
            data += chunk
        return data.decode()

    def command(self, value):
        self.process.stdin.write((str(value) + "\n").encode())
        self.output = self.read()
        assert "Rejected:" not in self.output, self.output
        return self.output

    def actions(self):
        return [(int(i), description) for i, description in
                re.findall(r"^\s*(\d+): (.*)$", self.output, re.M)]

    def finish(self):
        self.process.stdin.write(b"q\n")
        self.process.communicate(timeout=3)
        assert self.process.returncode == 0


def advance_to_buy(stepper, player, cursor=0):
    setup_resources = {1: set(), 2: set()}
    purchase_resources = {"pasture", "fields", "mountains"}
    for _ in range(300):
        choices = stepper.actions()
        acting = int(re.search(r"acting: Player (\d+)", stepper.output).group(1))
        target = next((i for i, label in choices if label == "buy development card"), None)
        if acting == player and target is not None:
            return target
        if "| initial settlement |" in stepper.output:
            # Cover all three purchase resources across each player's two
            # settlements; a changed seed layout must not strand them without grain.
            def setup_score(item):
                adjacent = {terrain for terrain in purchase_resources if terrain in item[1]}
                return (100 * len(adjacent - setup_resources[acting])
                        + 10 * len(adjacent) + item[1].count("H"))
            target, label = max(choices, key=setup_score)
            setup_resources[acting].update(terrain for terrain in purchase_resources if terrain in label)
        elif "| dice roll |" in stepper.output:
            rolls = [(i, label) for i, label in choices if label.startswith("roll ") and
                     not label.startswith("roll 7 ")]
            assert rolls
            target = rolls[cursor % len(rolls)][0]
            cursor += 1
        else:
            target = next((i for i, label in choices if label == "end turn"), choices[0][0])
        stepper.command(target)
    raise AssertionError("did not reach a development purchase")


def main():
    cli, renderer = sys.argv[1:]
    with tempfile.TemporaryDirectory(prefix="catan-information-cli-") as directory:
        for observer in (1, 2):
            stepper = Stepper(cli, f"p{observer}")
            try:
                choice = advance_to_buy(stepper, 3 - observer)
                output = stepper.command(choice)
                assert "belief updated" in output and "| main |" in output, output
                assert not any(label.startswith("draw ") for _, label in stepper.actions())
                snapshot = Path(directory) / f"p{observer}.json"
                stepper.command(f"json {snapshot}")
                board = json.loads(snapshot.read_text())
                assert board["information"]["observer"] == observer - 1
                assert board["information"]["world_count"] == 5
                assert board["information"]["opponent_card_count"] == 1
                opponent = board["players"][2 - observer]
                assert opponent["development_cards"] is None and opponent["total_vp"] is None
                assert board["development_deck"] is None
                assert abs(board["information"]["expected_opponent_cards"]["knight"] - 14 / 25) < 1e-12
                subprocess.run([sys.executable, renderer, "--input", str(snapshot),
                                "--output", str(snapshot.with_suffix(".svg"))], check=True,
                               capture_output=True)
                assert "Unknown development cards: 1" in snapshot.with_suffix(".svg").read_text()
                stepper.command("rewind")
                stepper.command(f"json {snapshot}")
                restored = json.loads(snapshot.read_text())
                assert restored["information"]["world_count"] == 1
                assert restored["information"]["opponent_card_count"] == 0
                stepper.command(choice)
                own_buy = advance_to_buy(stepper, observer)
                stepper.command(own_buy)
                assert "| development draw |" in stepper.output
                knight = next(i for i, label in stepper.actions() if label.startswith("draw knight"))
                stepper.command(knight)
                stepper.command(f"json {snapshot}")
                known = json.loads(snapshot.read_text())
                assert known["players"][observer - 1]["development_cards"]["knight"] == 1
                assert abs(known["information"]["expected_opponent_cards"]["knight"] - 13 / 24) < 1e-12
                result = stepper.command("mcts 30")
                assert f"MCTS from Player {observer} perspective" in result
            finally:
                stepper.finish()
        # Run the actual background workers and local viewer without opening a
        # GUI browser. Check the initial P2-observer case before anyone moves.
        for observer in (1, 2):
            stepper = Stepper(cli, f"p{observer}", live=True)
            try:
                url = re.search(r"Live visualizer: (http://127\.0\.0\.1:\d+/)", stepper.output).group(1)
                with urllib.request.urlopen(url + "image.svg", timeout=3) as response:
                    svg = response.read().decode()
                assert f'data-observer="{observer - 1}"' in svg
                assert f'data-tree-running-p{observer}="true"' in svg
                assert f'data-tree-running-p{3 - observer}="false"' in svg
                position = int(re.search(r'data-position-version="(\d+)"', svg).group(1))
                payload = json.dumps({"type": "tree", "position_version": position,
                                      "player": 2 - observer, "enabled": True}).encode()
                request = urllib.request.Request(url + "command", data=payload,
                                                 headers={"Content-Type": "application/json"})
                try:
                    urllib.request.urlopen(request, timeout=3)
                    raise AssertionError("unavailable opponent tree was acknowledged as accepted")
                except urllib.error.HTTPError as response:
                    assert response.code == 409
                    reply = json.load(response)
                    assert not reply["accepted"] and "opponent tree is unavailable" in reply["message"]
                assert "opponent tree is unavailable" in stepper.read()
                # Manual UI actions must work on opponent turns without an
                # MCTS action list, and preserve the CLI's transformation IDs.
                choice = advance_to_buy(stepper, 3 - observer)
                with urllib.request.urlopen(url + "state.json", timeout=3) as response:
                    public = json.load(response)
                assert public["mcts"] is None
                purchase = next(action for action in public["transformations"]
                                if action["description"] == "buy development card")
                assert purchase["index"] == choice
                payload = json.dumps({"type": "transform", "position_version": public["controls"]["position_version"],
                                      "action_index": purchase["index"]}).encode()
                request = urllib.request.Request(url + "command", data=payload,
                                                 headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(request, timeout=3) as response:
                    assert response.status == 200
                output = stepper.read()
                assert "Viewer recorded: buy development card" in output and "belief updated" in output
                with urllib.request.urlopen(url + "state.json", timeout=3) as response:
                    hidden = json.load(response)
                assert hidden["information"]["opponent_card_count"] == 1
                assert hidden["information"]["world_count"] == 5
                assert hidden["players"][2 - observer]["development_cards"] is None
                assert hidden["turn"]["phase"] == "main"
                assert not any(action["description"].startswith("draw ") for action in hidden["transformations"])
            finally:
                stepper.finish()
    bad = subprocess.run([cli, "--perspective", "p3"], capture_output=True)
    assert bad.returncode == 2
    print("Both information CLI perspectives, hidden purchases, own draws and rewind passed")


if __name__ == "__main__":
    main()
