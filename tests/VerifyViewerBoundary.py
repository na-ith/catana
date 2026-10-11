"""Exercise typed moves, acknowledgements and incremental live publications."""
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

from VerifyInformationCli import Stepper, advance_to_buy


def get(url, path):
    with urllib.request.urlopen(url + path, timeout=4) as response:
        return json.load(response)


def command(stepper, url, position, payload, accepted=True):
    request = urllib.request.Request(url + "command", data=json.dumps({
        "position_version": position["controls"]["position_version"], **payload,
    }).encode(), headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(request, timeout=4) as response:
            reply = json.load(response)
    except urllib.error.HTTPError as response:
        assert response.code == 409, response.read()
        reply = json.load(response)
    assert reply["accepted"] == accepted, reply
    assert reply["request_id"] > 0 and reply["message"], reply
    stepper.output = stepper.read()
    return reply


def main():
    for observer in (None, "p1", "p2"):
        stepper = Stepper(sys.argv[1], observer, live=True)
        try:
            url = re.search(r"Live visualizer: (http://127\.0\.0\.1:\d+/)", stepper.output).group(1)
            initial = get(url, "updates")
            position = initial["position"]
            assert "topology" not in position and "hexes" not in position["board"]
            assert len(initial["layout"]["topology"]["vertices"]) == 54
            assert "players" not in initial["layout"]
            choices = position["transformations"]
            assert len({move["id"] for move in choices}) == len(choices)
            move = next(move for move in choices if move["type"] == "place_settlement")
            assert isinstance(move["vertex"], int) and move["highlight_targets"] == [f"vertex-{move['vertex']}"]
            if observer:
                other = 2 - int(observer[-1])
                assert position["players"][other]["development_cards"] is None
                assert position["development_deck"] is None
            # Warm search updates must leave the layout and position publications
            # untouched and carry no descriptions or legal moves in score rows.
            query = urllib.parse.urlencode(initial["revisions"])
            deadline = time.monotonic() + 5
            if observer != "p2":
                while True:
                    delta = get(url, "updates?" + query)
                    assert "layout" not in delta and "position" not in delta
                    if delta.get("analysis", {}).get("mcts"):
                        stats = delta["analysis"]["mcts"]["actions"]
                        assert stats and all("id" in row and "description" not in row and "vertex" not in row for row in stats)
                        break
                    assert time.monotonic() < deadline
                    time.sleep(0.04)
            command(stepper, url, position, {"type": "move", "move_id": move["id"]})
            after = get(url, "state.json")
            assert after["board"]["buildings"][move["vertex"]]["owner"] == 0
            assert after["controls"]["position_version"] > position["controls"]["position_version"]
            # Old input and invented IDs receive explicit engine rejections.
            command(stepper, url, position, {"type": "move", "move_id": move["id"]}, accepted=False)
            command(stepper, url, after, {"type": "move", "move_id": "not-a-published-move"}, accepted=False)
            assert get(url, "state.json")["board"] == after["board"]
            road = next(move for move in after["transformations"] if move["type"] == "place_road")
            command(stepper, url, after, {"type": "move", "move_id": road["id"]})
            built = get(url, "state.json")
            assert built["board"]["roads"][road["edge"]]["owner"] == 0
            command(stepper, url, built, {"type": "rewind"})
            restored = get(url, "state.json")
            assert restored["board"] == after["board"]
            command(stepper, url, restored, {"type": "tree", "player": 1 if observer == "p2" else 0,
                                           "enabled": False})
            paused = get(url, "state.json")
            assert not paused["controls"]["tree_running"][1 if observer == "p2" else 0]
            assert paused["controls"]["position_version"] == restored["controls"]["position_version"]
            # Undo the arbitrary settlement used for move/rewind checks so the
            # purchase helper can choose productive setup for both players.
            command(stepper, url, paused, {"type": "rewind"})
            restarted = get(url, "state.json")
            assert restarted["board"]["buildings"] == position["board"]["buildings"]
            assert restarted["board"]["roads"] == position["board"]["roads"]
            # Cover native purchases and IDs with string parameters containing
            # spaces, including both known and automatically hidden draws.
            target = 2 if observer else 1
            advance_to_buy(stepper, target)
            buying = get(url, "state.json")
            buy = next(move for move in buying["transformations"] if move["type"] == "buy_development")
            command(stepper, url, buying, {"type": "move", "move_id": buy["id"]})
            purchased = get(url, "state.json")
            if observer == "p1":
                assert purchased["turn"]["phase"] == "main"
                assert purchased["information"]["opponent_card_count"] > 0
                assert purchased["players"][1]["development_cards"] is None
            else:
                draw = next(move for move in purchased["transformations"]
                            if move["type"] == "draw_development" and move["card"] == "road building")
                command(stepper, url, purchased, {"type": "move", "move_id": draw["id"]})
                purchased = get(url, "state.json")
                assert purchased["turn"]["phase"] == "main"
                assert purchased["players"][target - 1]["development_cards"]["road building"] > 0
            end_turn = next(move for move in purchased["transformations"] if move["type"] == "end_turn")
            command(stepper, url, purchased, {"type": "move", "move_id": end_turn["id"]})
            next_turn = get(url, "state.json")
            assert next_turn["turn"]["phase"] == "await roll"
            assert next_turn["turn"]["decision_player"] == 2 - target
        finally:
            stepper.finish()
    print("Typed moves, stale rejection, rewind, privacy and independent publications passed")


if __name__ == "__main__":
    main()
