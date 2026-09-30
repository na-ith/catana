# Colonist ranked 1v1 simulator

A deterministic, steppable C++20 game engine for Colonist-style ranked 1v1. The engine keeps decisions and chance outcomes separate so callers can manually inspect games now and later expand dice rolls, development-card draws, and steals as exact chance nodes.

## Build and step through a game

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./build/catan_cli 42
```

The CLI lists every legal action. At a chance node it adds a final synthetic `random` option; select its number or enter `random` to sample from the displayed weighted distribution (`auto` remains an alias). This debugging option is not added to the simulator or MCTS tree—the sampled concrete outcome is the action that gets applied. Enter `rewind` (or `undo`) to restore the complete state before the previous accepted action. Supplying the same board seed reproduces the same terrain, number, port layout, and random debugging selections.

At a player decision, enter `mcts [iterations]` to print the estimated win percentage and visit count for every legal action. Enter `best [iterations]` to run the same search and apply its highest-rated action. Dice rolls, development-card draws, and robber steals remain weighted chance outcomes rather than player choices.

The search layer is also available directly:

```cpp
#include "catan/Mcts.h"

catan::MctsConfig config;
config.iterations = 10'000;
config.maxRolloutSteps = 300;
config.seed = 42;

const auto result = catan::MctsEngine(config).search(game.state());
for (const auto& option : result.actions) {
    // option.action
    // option.estimatedWinProbability (0.0 to 1.0)
    // option.visits
}
if (result.bestAction) game.step(*result.bestAction);
```

`MctsEngine` searches the complete state passed to it. The live stepper layers player-specific development-card beliefs over that core: each player knows their own cards and searches weighted determinizations of the opponent's possible cards and the remaining deck. Opponent decision nodes minimize the searching player's result, while dice, development draws, and steals are sampled from `Game::chanceOutcomes()`. Rollouts that do not finish within the configured depth use a bounded position evaluation, so reported percentages are estimates rather than exact solved probabilities.

Intentional changes from minimal UCT—including action pruning, sibling-state folding, persistent trees, parallel workers, rejected optimisations, and their correctness assumptions—are maintained in [`docs/MCTS_DEVIATIONS.md`](docs/MCTS_DEVIATIONS.md).

### Live visualizer

Start the stepper with the visualizer flag:

```sh
./build/catan_cli --visualizer 42
# equivalent:
./build/catan_cli --seed 42 --visualizer
# override the automatic worker count:
./build/catan_cli --seed 42 --visualizer --mcts-threads 8
```

This starts a Python localhost viewer and opens it in a new browser window. The C++ process atomically publishes a new omniscient snapshot after every accepted action, and the viewer redraws automatically. The local viewer server is stopped when the stepper exits. If the browser cannot be opened automatically, the terminal prints the local URL to open manually.

When the visualizer is enabled, each player gets an independent fixed-perspective, root-parallel belief tree. Both run automatically while the CLI waits for input, and the **P1 tree** and **P2 tree** switches independently pause or resume their calculation. `--mcts-threads N` sets the workers per enabled player tree. A private development-card draw advances the owner's workers through the observed card while the other player's workers sample the hidden outcome; incompatible workers are rebuilt from the exact Bayesian posterior. Public actions retain compatible subtrees in both forests.

The analysis panel shows the acting player's belief tree, refreshes as work completes, ranks every current action by estimated win percentage, retains its original CLI index, shows combined visits, worker count, explored depth, and unique root nodes, and highlights the current recommendation. Hover an analysis row to highlight its corresponding board or state element; click it to apply that action. At chance nodes, **random weighted outcome** samples a concrete result without adding a synthetic MCTS branch. **Rewind one action** restores the complete game, debugging RNG, and both development-card beliefs. Stale clicks from older positions are ignored. Rewinding starts fresh search trees at the restored beliefs because discarded ancestors are not retained.

### Render the current state

At any step, enter:

```text
image
image snapshots/turn-12.svg
json snapshots/turn-12.json
```

`image` creates a dependency-free SVG containing the board, engine hex/vertex/edge IDs, roads, buildings, ports, robber, both players' omniscient hands, bank, awards, turn phase, and balanced-dice state. The Python renderer lives at `tools/render_state.py` and can also be used directly:

```sh
python3 tools/render_state.py --input state.json --output state.svg
```

Code can invoke the same layer without depending on the CLI:

```cpp
#include "catan/Debug.h"

std::string error;
catan::debug::renderStateImage(game.state(), "state.svg", &error);
```

Long-running integrations can use `catan::debug::LiveVisualizer` and call `update()` after applying an action.

The debug snapshot is deliberately separate from the engine and is not a stable save-game format.

### Capture Colonist WebSocket messages

`tools/capture_colonist_ws.mjs` attaches to Chrome through its local debugging
protocol, records the Colonist game socket, and decodes its binary MessagePack
frames into readable JSONL. It records frame payloads but not cookies or HTTP
authentication headers. Node.js 22 or newer is required; there are no package
dependencies.

Start a separate Chrome instance on macOS:

```sh
open -na "Google Chrome" --args \
  --remote-debugging-port=9222 \
  --user-data-dir=/tmp/colonist-capture-profile
```

Open Colonist in that browser, leave it in the lobby, then start the recorder:

```sh
node tools/capture_colonist_ws.mjs --output colonist-game.jsonl
```

Once it reports that it is attached, reload Colonist and join or resume the
game. Each frame is printed in compact form as it arrives and appended to the
JSONL file with both its raw payload and decoded value. Use `--verbose` to print
complete decoded values in the terminal. Stop cleanly with `Ctrl-C`.

The capture may contain player identifiers or other private game data. Review
it before sharing it.

## Ranked-rule verification

The custom unit-test runner covers setup, ranked-only rules, production, bank shortages, construction, ports, robber flows, every development card, Bayesian hidden-card tracking, MCTS, awards, balanced dice, victory, invariants, and deterministic playouts:

```sh
ctest --test-dir build --output-on-failure
./build/catan_tests
./build/catan_tests balanced_dice
ctest --test-dir build --output-on-failure -L system
```

The rule-to-test matrix and explicit limits of public Colonist parity are documented in [`tests/RULE_COVERAGE.md`](tests/RULE_COVERAGE.md).

## Implemented rules

- snake-order initial placements and second-settlement resources;
- production, bank shortages, robber blocking, random steals, and discarding;
- roads, settlements, cities, distance/connectivity rules, piece limits, and ports;
- all base development cards, including the one-card-per-turn and newly-bought restrictions;
- longest road (including branches, cycles, and interruption), largest army, and 15-point victory;
- ranked 1v1 differences: no player trading, discard limit 9, friendly robber through 2 public points, and balanced dice;
- Colonist's published balanced-dice deck, recent-roll suppression, and 1v1 seven balancing.

The simulator stores the authoritative omniscient state. `DevelopmentBelief` provides each search player with a separate joint posterior over hidden opponent cards and the remaining development deck.

## Rule references

- [Colonist base-game rules](https://colonist.io/catan-rules)
- [Colonist ranked 1v1 rules and friendly robber details](https://blog.colonist.io/ranked-1v1-comprehensive-strategy-guide-colonist-io/)
- [Colonist balanced-seven explanation](https://blog.colonist.io/balancing-7s-on-1v1/) and its [published implementation](https://pastebin.com/wWH4Ka96)

## Layout

- `Types.h`: IDs, enums, resources, and costs
- `Board.h`: immutable topology plus board pieces and randomized layout
- `State.h`: complete serializable game state
- `Action.h`: player and chance actions
- `Rules.h`: pure legality, scoring, port, and longest-road queries
- `Game.h`: legal-action generation and state transitions
- `Mcts.h`: adversarial Monte Carlo tree search and root-action win estimates
- `Belief.h`: player-specific Bayesian development-hand/deck hypotheses
- `Debug.h`: optional JSON snapshot and Python-renderer adapter
- `tools/render_state.py`: standalone SVG renderer
