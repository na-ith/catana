"""Execute a real MCTS trade batch through both CLI and viewer, then rewind."""
import json
import re
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

from VerifyInformationCli import Stepper, advance_to_buy


def read_snapshot(url):
    with urllib.request.urlopen(url + "state.json", timeout=3) as response:
        return json.load(response)


def reach_trade(stepper):
    advance_to_buy(stepper, 1)
    cursor = 0
    for _ in range(200):
        choices = stepper.actions()
        if "| main | acting: Player 1" in stepper.output and any(label.startswith("trade ") for _, label in choices):
            return
        if "| dice roll |" in stepper.output:
            rolls = [(i, label) for i, label in choices if re.match(r"roll \d+\b", label)
                     and not re.match(r"roll 7\b", label)]
            move = rolls[cursor % len(rolls)][0]
            cursor += 1
        else:
            move = next((i for i, label in choices if label == "end turn"), choices[0][0])
        stepper.command(move)
    raise AssertionError("did not reach a tradable main phase")


def main():
    for observer, live in ((None, False), ("p1", True)):
        stepper = Stepper(sys.argv[1], observer, live=live)
        try:
            url = re.search(r"Live visualizer: (http://127\.0\.0\.1:\d+/)", stepper.output).group(1) if live else None
            reach_trade(stepper)
            if live:
                deadline = time.monotonic() + 5
                while True:
                    before = read_snapshot(url)
                    choices = before["mcts"]["actions"] if before["mcts"] else []
                    batch = next((action for action in choices if action["description"].startswith("trade batch (2)")),
                                 next((action for action in choices if action["description"].startswith("trade batch")), None))
                    if batch:
                        break
                    assert time.monotonic() < deadline, before
                    time.sleep(0.05)
                assert "bank" in batch["highlight_targets"]
                payload = json.dumps({"type": "action", "position_version": before["controls"]["position_version"],
                                      "action_index": batch["index"]}).encode()
                request = urllib.request.Request(url + "command", data=payload,
                                                 headers={"Content-Type": "application/json"})
                with urllib.request.urlopen(request, timeout=3) as response:
                    assert response.status == 200
                stepper.output = stepper.read()
                assert "Viewer selected" in stepper.output and "trade batch" in stepper.output
                assert "Rejected:" not in stepper.output
                after = read_snapshot(url)
                assert after["players"][0]["resources"] != before["players"][0]["resources"]
                assert after["controls"]["position_version"] > before["controls"]["position_version"]
                stepper.command("rewind")
                restored = read_snapshot(url)
                assert restored["players"] == before["players"]
                assert restored["bank"] == before["bank"]
                assert restored["turn"] == before["turn"]
            else:
                with tempfile.TemporaryDirectory(prefix="catan-trade-cli-") as directory:
                    path = Path(directory) / "state.json"
                    stepper.command(f"json {path}")
                    before = json.loads(path.read_text())
                    stepper.command("mcts 50")
                    choice = next(i for i, label in stepper.actions() if label.startswith("trade batch"))
                    stepper.command(choice)
                    assert "-> trade batch" in stepper.output
                    stepper.command(f"json {path}")
                    after = json.loads(path.read_text())
                    assert after["players"][0]["resources"] != before["players"][0]["resources"]
                    stepper.command("rewind")
                    stepper.command(f"json {path}")
                    restored = json.loads(path.read_text())
                    assert restored["players"] == before["players"]
                    assert restored["bank"] == before["bank"]
        finally:
            stepper.finish()


if __name__ == "__main__":
    main()
