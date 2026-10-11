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

Search groups consecutive bank trades into a `trade batch` and keeps one
sequence per resulting hand. Select a batch from the CLI's MCTS results or
select its viewer analysis row and commit to apply all exchanges; one rewind undoes the
whole batch. Ordinary single trades remain available for manual stepping.
Build/trade orderings are removed only when replay proves them equivalent,
preserving new ports and bank shortages. Development-card purchases and plays
reopen trading. Very large trade spaces fall back to individual trades rather
than omit choices; the enumeration budget is `MctsConfig::maxTradeBatchStates`.
Details and rollout-policy changes are in [MCTS_DEVIATIONS.md](docs/MCTS_DEVIATIONS.md).

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
if (result.bestAction) game.transform(*result.bestAction);
```

### Benchmark MCTS

Run the optimized benchmark suite from the repository root:

```sh
python3 tools/benchmark_mcts.py
```

The runner builds a separate Release benchmark target under `build/mcts-benchmark`
and reports median search time and iterations per second across repeated runs.
It covers initial settlement and road placement, a dice chance node, early and
midgame decisions, a resource-rich trading position, robber decisions, and
simultaneous discards, plus an information-state search with an unknown
opponent development card. Scenario setup is outside the timed search. Save raw
measurements with `--csv`:

```sh
python3 tools/benchmark_mcts.py --iterations 5000 --repeats 5 --csv results.csv
```

Use `--scenario NAME` to run one case, `--rollout-steps N` to change the rollout
limit, `--seed N` to reproduce board fixtures, or `--no-build` to reuse the
existing benchmark binary. Run the same options before and after a code change
for comparable results.

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

This starts a Python localhost viewer and opens it in a new browser window. Select **Settlement**, **Road**, **City**, or **Robber** to highlight legal board targets. Hover or focus a target to preview the move, then click or press Enter to commit it. Setup and mandatory robber/road phases automatically select their tool. **Board IDs** shows engine IDs when needed. The local viewer server stops when the stepper exits; if the browser cannot open automatically, the terminal prints the local URL.

The move panel covers dice totals, development-card purchases and plays, draws,
steals, Year of Plenty, Monopoly, end turn, and resignation. Select an action
and click **Commit move**. If only one normal move is available (such as rolling
the dice or ending the turn), selecting it applies it immediately. Resignation
always requires confirmation. Bank trading highlights legal receiving resources
after you select what to give, showing the current port exchange rate. Discards
use resource counters and enable selection only for a valid complete discard.
In perspective mode these controls record observed moves and outcomes.

The compact **Game events** panel records confirmed moves beside the board.
Each dice result starts a fresh **This turn** history and shows how much of each
resource each player received. Subsequent moves append to that history; new
entries are highlighted briefly and remain available until the next dice result.
Rewind restores the previous history along with the game. Hidden opponent
development-card draws remain unknown in perspective mode.

When the visualizer is enabled, each player gets an independent fixed-perspective, root-parallel belief tree. Both run automatically while the CLI waits for input, and the **P1 tree** and **P2 tree** switches independently pause or resume their calculation. `--mcts-threads N` sets the workers per enabled player tree. A private development-card draw advances the owner's workers through the observed card while the other player's workers sample the hidden outcome; incompatible workers are rebuilt from the exact Bayesian posterior. Public actions retain compatible subtrees in both forests.

The analysis panel shows the acting player's belief tree, win estimates,
visits, worker count, explored depth, and unique root nodes. Hover a row to
highlight its board or hand targets; select it and click **Commit move** to
apply it, including atomic trade batches. Scores refresh without replacing the
board or input controls. Ranking holds while the list has pointer or keyboard
focus. Large lists initially show 80 moves; **Show more moves** reveals the rest.
**Sample random outcome** samples weighted chance results for debugging.
**Rewind** restores the complete game, debugging RNG, and both development-card
beliefs. Stale clicks receive an explicit rejection. Rewinding starts fresh
search trees because discarded ancestors are not retained.

The live boundary publishes immutable layout once, position and structured
legal moves when input changes, and compact MCTS scores separately. Extra search
moves such as trade batches get a catalog update only when first published.
The Python server caches these publications; `/updates` returns only components
whose revisions differ from the browser's. The browser builds SVG geometry once
and updates pieces, controls, and keyed analysis rows independently. Commands
submit a typed move's stable ID and position version, and wait for a C++ engine
acknowledgement. The file transport and Python standard-library server need no
additional runtime dependencies. `/state.json` and `/image.svg` still assemble
full debug exports on demand; legacy index commands remain supported.

If the page reports an older viewer interface, rebuild the executable you are
launching and restart the stepper. Refreshing the browser does not update an
already running C++ process. For the default build, use
`cmake --build build --target catan_cli`, then launch `./build/catan_cli` again.

### Play from one player's information

Use `--perspective p1` or `--perspective p2` to follow a real game with only
that player's private development cards available:

```sh
./build/catan_cli 42 --perspective p1 --visualizer
./build/catan_cli --board colonist-game.jsonl --perspective p2 --visualizer
```

Only the selected player's forest is created and run. The opponent's tree
switch is disabled. The board and move controls expose legal transformations
even when no MCTS analysis is available. On the opponent's turn, select what
they actually did and click **Record move** (or enter its number in
the CLI). Selecting **buy development card** automatically resolves an
unknown draw into a Bayesian distribution; it never asks for or invents an
authoritative opponent card. A subsequent observed card play removes
incompatible possibilities, while other unplayed cards can remain uncertain.
On your own purchase, select the card you actually received at the development
draw step. Enter public dice and steal outcomes as observed, too.
The viewer lets you select and commit observed outcomes, and offers **Sample
random outcome** for debugging. The perspective and acting player are shown
above the board; you can still click MCTS recommendations on your own turn.

Resources are known for both players. The CLI, JSON exports and viewer mask
opponent development-card identities and total private VP. The viewer shows
expected opponent card counts and expected deck composition, labeled as
estimates. `rewind` restores the complete information distribution; an
opponent purchase and its automatic unknown draw rewind together. Use
`winner p1` or `winner p2` to report a victory consistent with the possible
worlds, including a hidden VP draw that ended the game.

Without the flag, the original full-game debugging mode still runs both
observer forests. The seeded simulator selects the real development draws,
but each forest receives only observations available to its player.

The engine entry point accepts `InformationState` directly:

```cpp
catan::InformationState information(initialGame.state(), catan::PlayerId::One);
catan::InformationMctsSession tree(information, config);
auto evaluation = tree.run();
// For an opponent purchase, apply both observations; no card identity needed.
tree.transform(catan::BuyDevelopmentCard{});
tree.transform(catan::DrawUnknownDevelopmentCard{});
```

The initial position is a known prior, normally the empty board before setup.
`InformationState::state()` is a masked display projection, not a complete
`GameState` to feed back into the simulator. `legalTransformations()`,
`chanceOutcomes()` and `transform()` operate on the whole joint distribution.
`MctsEngine::search(information)` supports a non-persistent search as well.
The search still uses sampled complete-world simulations internally; this
interface change does not implement full information-set MCTS.

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

### Initialize the CLI from a Colonist board

The CLI has an explicit uninitialized mode. It creates no game, viewer, or search
tree until it receives and validates a complete board:

```sh
./build/catan_cli --uninitialized --visualizer
```

Its stdin initialization API consists of one command per line:

```text
status
init /path/to/colonist-game.jsonl
```

`init` accepts the capture tool's JSONL, a decoded Colonist message or full
snapshot JSON, or the `catan-board-v1` format described below. Paths may contain
spaces; pass the path directly after `init` without adding shell quotes. A failed
import leaves the CLI uninitialized and allows retry. Once successful, the normal
stepper commands become available and the requested visualizer and background
MCTS start. A seeded launch is still available as before.

A webpage integration can also send a compact JSON message directly over stdin:

```text
init-json {"id":123,"data":{"gameState":{"mapState":{...}}}}
```

The `...` above represents the complete captured fields, not a literal input.
`init-json` expects the entire JSON on one line. It accepts the same wrappers as
file import, including a capture record containing `decoded.value`. Inline import
currently requires macOS or Linux; file import is available through the same API.

To start immediately from an existing capture:

```sh
./build/catan_cli --board colonist-game.jsonl --visualizer
```

`--seed` cannot be combined with `--uninitialized` or `--board`. Imported games
use a fixed internal random seed for reproducible debug chance selections and
search; it does not generate or alter the imported layout.

This API imports **board layout** and starts a fresh game before Player 1's first
settlement. It takes terrain, number tokens, and port locations/types. Existing
pieces, hands, turn state, and dice history in a capture are not restored; the
initial robber is placed on the desert. For a multi-game capture, the first
complete supported layout is used. Partial `diff` messages alone are insufficient.

The adapter supports standard radius-two boards with `mapState.tileHexStates`
and `mapState.portEdgeStates`, represented as indexed objects or arrays. Tiles
use Colonist's `x`, `y`, `type`, and `diceNumber`; ports use `x`, `y`, `z`, and
`type`. Terrain enums are 0=desert, 1=lumber, 2=brick, 3=wool, 4=grain, 5=ore;
sea tiles (8) are ignored and expansion terrain is rejected. Ports use a separate
enum: 1=generic, 2=lumber, 3=brick, 4=wool, 5=grain, 6=ore. Tile IDs
and port IDs are translated by coordinates rather than treated as engine IDs.

Other clients can supply a stable normalized format:

```json
{
  "schema": "catan-board-v1",
  "hexes": [{"id": 0, "terrain": "forest", "number": 6}],
  "ports": [{"edge": 7, "type": "generic"}],
  "robber_hex": 18
}
```

That illustration abbreviates the arrays: a valid input needs all 19 unique
engine hex IDs and nine ports, the standard terrain/token/port inventories,
non-overlapping coastal port edges, and the desert's actual ID. Terrain names
are `forest`, `hills`, `pasture`, `fields`, `mountains`, `desert`; port names
are `generic`, `lumber`, `brick`, `wool`, `grain`, `ore`. Engine IDs and vertex
connections are available in the existing debug snapshots.

The adapter can also print normalized JSON with the source-to-engine ID mapping:

```sh
python3 tools/import_colonist_board.py colonist-game.jsonl
```

For C++ callers, link `catan_import` and use the same validated boundary:

```cpp
#include "catan/BoardImport.h"
#include "catan/Game.h"

std::string error;
if (auto board = catan::loadColonistBoard("colonist-game.jsonl", &error)) {
    catan::Game game(*board);
    // Ready for initial placement with normal bank, deck, and balanced dice.
}
// loadColonistBoardJson(decodedJson, &error) accepts inline JSON instead.
```

JSON decoding is handled by the standalone Python adapter using its standard
library; the engine also validates the resulting typed `BoardState`. Captured
layout behavior is covered by fixture-based end-to-end tests; a live Colonist
capture has not yet been supplied to verify the webpage's current payload.

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

Generated boards approximate Colonist's normal **Base** map: terrain is shuffled,
then the standard A-R number sequence is placed in a counterclockwise spiral
from one of six randomly chosen corners, skipping the desert. This keeps red
numbers (6 and 8) apart without rejection sampling. Port types are shuffled
among the existing fixed coastal locations.

The spiral is an inference from the standard Catan setup,
Colonist's [six Base-map orientations](https://colonist.io/patch-notes) (January 5,
2021), and its separate [Base Random map](https://colonist.io/store). Colonist's
exact generation algorithm and port distribution are unverified. This generator
does not model Base Random. Seeds remain reproducible, but generated layouts for
existing seeds change with this update; imported layouts are preserved.

Discards after a seven resolve simultaneously. In the CLI or live viewer,
record your discard first, then the opponent's observed discard. Neither hand
changes until all required choices have been supplied. Rewind cancels pending
selections; after resolution, one rewind restores both original hands.

For C++ callers, use `Game::discardChoices(player)` or
`InformationState::discardChoices(player)` for each required participant, then
call `transform(ResolveDiscards{...})` with both resource vectors (zero for a
player who owes nothing). Complete pairs are validated directly, rather than
enumerating their potentially large Cartesian product. `DiscardResources`
alone is accepted only when exactly one player owes cards. MCTS evaluates your
options using a uniform opponent-discard policy from the pre-discard state;
the approximation is recorded in [`docs/MCTS_DEVIATIONS.md`](docs/MCTS_DEVIATIONS.md).

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
- `tools/viewer.html`, `viewer.js`, `viewer.css`: interactive live board and move controls
- `BoardImport.h`: validated Colonist board file/JSON import API
- `tools/import_colonist_board.py`: decoded webpage/capture layout adapter
- `tools/capture_colonist_ws.mjs`: browser WebSocket recorder and MessagePack decoder
