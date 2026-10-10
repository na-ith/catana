"""Atomic discards, input staging, and rewind through the real live stepper."""
import json
import re
import sys
import time
import urllib.request

from VerifyInformationCli import Stepper


def snapshot(url):
    with urllib.request.urlopen(url + "state.json", timeout=3) as response:
        return json.load(response)


def viewer_move(stepper, url, state, index):
    move = next(move for move in state["transformations"] if move["index"] == index)
    payload = json.dumps({"type": "move", "position_version": state["controls"]["position_version"],
                          "move_id": move["id"]}).encode()
    request = urllib.request.Request(url + "command", data=payload,
                                     headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=3) as response:
        assert response.status == 200
    stepper.output = stepper.read()
    assert "Rejected:" not in stepper.output, stepper.output
    return snapshot(url)


def reach_joint_discard(stepper):
    cursor = 0
    for _ in range(600):
        choices = stepper.actions()
        if "| discarding |" in stepper.output:
            return
        if "| initial settlement |" in stepper.output:
            move = max(choices, key=lambda item: sum(terrain in item[1] for terrain in
                       ("pasture", "fields", "mountains")) * 10 + item[1].count("H"))[0]
        elif "| dice roll |" in stepper.output:
            hands = re.findall(r"resources \[([^]]+)\]", stepper.output)
            ready = len(hands) == 2 and all(sum(map(int, re.findall(r"=(\d+)", hand))) > 9 for hand in hands)
            seven = next((i for i, label in choices if re.match(r"roll 7\b", label)), None)
            if ready and seven is not None:
                move = seven
            else:
                rolls = [(i, label) for i, label in choices if re.match(r"roll \d+\b", label)
                         and not re.match(r"roll 7\b", label)]
                move = rolls[cursor % len(rolls)][0]
                cursor += 1
        else:
            move = next((i for i, label in choices if label == "end turn"), choices[0][0])
        stepper.command(move)
    raise AssertionError("did not reach joint discard")


def main():
    for observer in (None, "p1", "p2"):
        stepper = Stepper(sys.argv[1], observer, live=True)
        # More menu text is possible when hands are large, but the test avoids
        # purchases and builds so all resources come from genuine production.
        stepper.deadline = time.monotonic() + 60
        try:
            url = re.search(r"Live visualizer: (http://127\.0\.0\.1:\d+/)", stepper.output).group(1)
            reach_joint_discard(stepper)
            before = snapshot(url)
            assert all(n > 0 for n in before["turn"]["pending_discards"])
            expected_actor = 1 if observer == "p2" else 0
            assert before["controls"]["discard_input_player"] == expected_actor
            original_hands = [p["resources"] for p in before["players"]]
            chosen = before["transformations"][0]
            assert chosen["description"].startswith("discard [")
            waiting = viewer_move(stepper, url, before, chosen["index"])
            assert "Game state unchanged" in stepper.output
            assert [p["resources"] for p in waiting["players"]] == original_hands
            assert waiting["turn"] == before["turn"]
            assert waiting["controls"]["discard_input_player"] == 1 - expected_actor
            assert waiting["controls"]["discard_selected"][expected_actor]
            assert waiting["controls"]["position_version"] > before["controls"]["position_version"]
            assert waiting["controls"]["can_rewind"]
            if observer:
                assert waiting["mcts"] is None  # Cannot command the opponent tree.
            stepper.command("rewind")
            cancelled = snapshot(url)
            assert "Cancelled pending discard" in stepper.output
            assert cancelled["controls"]["discard_selected"] == [False, False]
            assert cancelled["controls"]["discard_input_player"] == expected_actor
            assert [p["resources"] for p in cancelled["players"]] == original_hands
            waiting = viewer_move(stepper, url, cancelled, chosen["index"])
            second = waiting["transformations"][0]
            resolved = viewer_move(stepper, url, waiting, second["index"])
            assert resolved["turn"]["phase"] == "move robber"
            assert resolved["turn"]["pending_discards"] == [0, 0]
            for player, action in ((expected_actor, chosen), (1 - expected_actor, second)):
                discarded = list(map(int, re.search(r"\[([^]]+)\]", action["description"]).group(1).split(",")))
                for resource, amount in zip(original_hands[player], discarded):
                    assert resolved["players"][player]["resources"][resource] == original_hands[player][resource] - amount
            stepper.command("rewind")
            restored = snapshot(url)
            assert restored["turn"] == before["turn"]
            assert [p["resources"] for p in restored["players"]] == original_hands
            assert restored["bank"] == before["bank"]
            assert restored["controls"]["discard_selected"] == [False, False]
        finally:
            stepper.finish()


if __name__ == "__main__":
    main()
