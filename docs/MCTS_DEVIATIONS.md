# MCTS deviations and search-policy decisions

This file records intentional differences between the engine and a minimal,
single-threaded UCT implementation. Add an entry whenever search changes the
legal branch set, statistics, rollout policy, tree topology, or execution
model. The simulator's `Game::transform()` remains the rules authority;
search-only pruning must not make an action illegal in the simulator.

## Implemented

### Consecutive bank-trade batches and build/trade canonical ordering

At main-phase nodes, search replaces individual maritime trades with
`MaritimeTradeBatch`: an ordered sequence executed atomically. Breadth-first
enumeration keeps one shortest legal sequence for every reachable resource
hand, including each possible stopping point. The bank follows uniquely from
the original hand and resource conservation, so another trade order reaching
the same hand has the same complete state. Each intermediate exchange must
be affordable and the receiving resource must be available in the bank;
bundles are not validated using only net resource deltas. Exchange rates stay
fixed because there is no building or information event inside a batch.

A batch cannot immediately follow another batch in the optimized tree:
their combined sequence is already represented at the preceding trading
position. The normal simulator still permits individual trades, and accepts
arbitrary valid ordered batches without requiring canonical spelling. An
observed alternate batch order can reuse its canonical explored result.

Search retains path metadata for a segment containing only trades and paid
builds. A post-build batch is omitted **only** if replaying the earlier trades
plus that batch before all intervening builds is legal and produces exactly
the same complete final state. This removes both `build -> trade` and
`trade -> build -> trade` permutations when interchangeable. It preserves
new port discounts, bank replenishment by building costs, and any other
case where replay is invalid or differs. After a necessary post-build trade,
a new ordering segment begins; earlier exceptional builds are not crossed.

Every purchase, development-card play/resolution, dice/robber/discard action,
or turn boundary resets this metadata. There is no blanket requirement to
play development cards before building: Knight can resolve a random steal,
Monopoly and Year of Plenty change resources, Road Building changes the
board, and purchases reveal a card before subsequent decisions. New cards
remain subject to the normal same-turn play restriction.

Exhaustive batching can increase immediate branching even while reducing
depth and duplicate paths. `MctsConfig::maxTradeBatchStates` defaults to 512
(including the starting hand). If enumeration would exceed this budget,
search discards the incomplete list and retains normal individual trades at
that node, including consecutive trades. Setting the budget to zero disables
tree batching. This is a completeness-preserving fallback, not truncation of
large bundles. Build/trade replay checks still apply to those single trades.
For eight lumber with no port, 20 nonempty ordered trade prefixes become 14
distinct trade-result edges. This example demonstrates deduplication, not a
measured general simulation-throughput gain.

Rollouts avoid exhaustive enumeration: after randomly selecting a first
legal trade, they append another random legal exchange with probability 0.5
until stopping or no exchange is possible, then apply the whole batch as one
rollout step. They use the same build-order replay checks and do not choose
another batch without a boundary/build. This changes the rollout policy and
the meaning of bounded rollout depth. Search depth also counts one batch as
one move, irrespective of its number of constituent exchanges.

These constraints live in search metadata, not `GameState` or the player's
beliefs. Identical-state sibling folding additionally compares this metadata
so children with different permitted continuations are not incorrectly merged.
Manually entering a legal move omitted by the optimized tree starts a fresh
root, as with other search-only pruning. The CLI and viewer apply batch
recommendations atomically; one rewind restores the entire batch.

### Simultaneous discard decisions and opponent-policy sampling

A seven creates a single pre-discard position. Each player's vectors are
obtained with `discardChoices(player)`; `ResolveDiscards` validates both
against that position and changes both hands and the bank in one transformation.
The Cartesian product is not enumerated by `legalTransformations()`. The CLI
and viewer stage input outside the state and commit only the complete pair.

When both players owe discards, each perspective's tree branches on that
perspective's own vector. Beneath it, an internal policy node samples the
opponent's legal vectors uniformly. Its distribution uses only the unchanged
pre-discard hand, never the selected own vector or a half-discarded hand.
The policy node has no simulator transformation; its children apply the
complete `ResolveDiscards`. Rollouts likewise select both vectors independently
from the same original position. Actual observed pairs re-root through these
nodes, retaining the matching explored child when available.

This uniform opponent-discard model is a deliberate approximation, not an
inherent chance rule and not an optimal simultaneous-game solver. Reported
values depend on that policy. Root discard choices are shown as decisions,
without chance percentages. Search depth currently includes the internal
policy level, although only its resolution changes the game state. If only
one player owes a discard, it remains an ordinary decision node.

### Adversarial opponent nodes

Opponent decision nodes select against the search perspective instead of
maximising the root player's value. Catan is a two-player adversarial game, so
treating both players as cooperative would inflate estimates and produce
obviously exploitable choices.

### Explicit weighted chance nodes

Dice rolls, development-card draws, and robber steals use the probabilities
from `Game::chanceOutcomes()`. An iteration samples those probabilities rather
than selecting chance outcomes with UCT. This preserves the simulator's chance
distribution. It is sampling, not yet exact chance-node evaluation.

### Bounded rollouts and heuristic leaves

Rollouts stop after `MctsConfig::maxRolloutSteps`. Unfinished games use a
bounded evaluator based on victory points, resources, played knights, road
length, and unplayed development cards. This keeps iteration time bounded, but
means the displayed win percentages are estimates rather than solved values.

### Resignation pruning

`Resign` remains a legal simulator operation but is omitted from expanded
nodes and random rollouts. In this win/loss model it cannot improve the acting
player's win probability over continuing, and exploring it spends visits on a
known zero-value branch.

### Low-production initial-settlement pruning (heuristic)

During all four initial settlement placements, search omits vertices touching
at most one resource-producing hex. Desert and absent sea faces do not count;
two hexes of the same resource count as two producing hexes. The goal is to
reduce setup branching by excluding locations that usually have poor income.
This rule does not apply to settlements built later in the game.

This is not a dominance proof: a valuable port, high-probability number,
resource scarcity, or a blocking/expansion plan can make an excluded placement
useful. Estimates therefore describe this restricted search policy.

### Robber-target pruning (heuristic)

Search omits robber destinations adjacent to the acting player's buildings
but no opponent buildings. Once the opponent has more than two public victory
points (the simulator's friendly-robber cutoff), search additionally omits
destinations with no opponent settlement or city. Mixed-ownership hexes remain
available. An opponent's empty hand does not exclude a target: blocking their
production can still be valuable. While friendly protection is active, legal
unoccupied hexes remain available, subject to the simulator's targeting rules.

These shortcuts favor immediately disruptive robber placements and reduce
branching. They can exclude strategic placements, including vacating a more
valuable self-blocked hex, preserving cooperation on a shared hex, or
anticipating future construction. They are not proven optimality-preserving.

Both heuristics apply to tree expansion and random rollouts, for either player
and both belief forests. They use public board information only. Simulator
legality and the debugging stepper are unchanged. If pruning would leave a
decision with no continuation, search restores its legal non-resigning actions
so unusual positions still have a usable tree. Manually selecting an omitted
action uses the existing fresh-root fallback described below.

### Zero-yield Monopoly pruning

A `PlayMonopoly` choice is omitted when the opponent has zero cards of the
selected resource. It only consumes the development card and the once-per-turn
development-card allowance; it cannot alter any beneficial game field. The
action remains legal in the simulator.

This proof relies on the current perfect-information state. A future
information-set or Bayesian search must not inspect the opponent's true hidden
hand when deciding whether the acting player considers this action useful. At
that point this prune must operate on beliefs or be removed.

### Safe pre-roll development-card deferral

Pre-roll Road Building is omitted unless an exact look-ahead over its first and
second free-road placements finds a line that claims Longest Road. Roads do not
affect production, discards, or a rolled robber, so other road sequences remain
available after the roll with the benefit of knowing its outcome. Longest Road
claims are retained as an explicit strategic exception, including claims that
immediately end the game.

Pre-roll Year of Plenty is omitted only when the bank can cover the maximum
possible production payout for every resource and still leave two cards of
that resource. In that case the player can reproduce the same two selections
after every non-seven roll, while waiting through a seven is also at least as
good after discards. The action is retained when any bank resource is scarce:
taking scarce cards before rolling can reserve them or deliberately cause a
production shortage, so blanket pruning would be unsound.

### Identical sibling-state folding

At deterministic decision nodes, different actions whose complete successor
`GameState`s compare equal share one child node and one set of statistics. One
canonical edge participates in UCT, while action aliases remain in reported
results. Chance-node edges are not folded because merging chance outcomes also
requires probability aggregation, and apparently similar dice outcomes retain
different dice-history state.

This is local sibling folding, not a global transposition table. It does not
yet merge states reached through different action orders, such as reversed
Year of Plenty picks, reversed Road Building placements, or independent main-
phase actions performed in a different order.

### Persistent re-rooting

After the real game applies an action, each `MctsSession` promotes the selected
child to its new root and retains that child's visits and descendants. A
default session follows the decision player and inverts retained values when
that perspective changes; an explicitly constructed player-perspective session
stays fixed across both players' turns. This avoids throwing away relevant
work between moves. If the real action was deliberately
pruned from search but remains simulator-legal, the session applies it and
starts a fresh tree at the resulting state; the stepper is therefore not
restricted to the search policy's branch set.

The debugging stepper can rewind to any earlier stored action state one step at
a time. Rewind starts new MCTS sessions at the restored position; forward
sessions intentionally do not retain discarded ancestors.

### Root-parallel workers

The live analyser runs independent persistent trees on several threads and
combines root action values using visit-weighted means. Workers do not share
internal nodes, which avoids synchronization in the hot path at the cost of
duplicating some exploration.

### Continuous background search

With the visualiser enabled, workers continue adding iterations while the CLI
waits for input. This changes the execution model rather than UCT semantics and
lets estimates improve live.

### Player-specific development-card belief trees

The live analyser uses `InformationMctsSession`, whose game-facing boundary is
one `InformationState`. Without a CLI perspective flag, there is a forest for
each player; `--perspective p1|p2` creates only the selected observer's forest.
Resources are known for both players. The joint development-card possibilities
retain correlation between hidden hands, card age, and the remaining deck.
Perspective mode stores no single authoritative opponent development hand.

When the observer buys a card, their belief conditions every hypothesis on the
card they privately observed. When the opponent buys a card, the observer's
belief branches over every feasible card and merges identical resulting
hypotheses. Public card plays eliminate incompatible worlds. Normal public
actions advance both forests, and compatible worker trees retain their
subtrees. Each worker samples a world from the current posterior for every
iteration and keeps a persistent complete-world tree for each world sampled.
Incompatible trees are discarded after an observation. Hidden opponent draws
use the observation-only `DrawUnknownDevelopmentCard` transformation; the CLI
automatically applies it after an opponent purchase. Own draws condition on a
concrete observed card. Compatible sampled draw children may be retained;
worlds not retained are explored lazily from the updated posterior.

Within a worker, root action values are weighted by current posterior mass
(and observation likelihood for chance outcomes), with a neutral 0.5 prior
for unexplored worlds. This prevents old visit allocation from masquerading as
a new posterior after private information is revealed. Separate workers still
combine action estimates using visit-weighted means. Observed card-play
feasibility conditions the posterior, but no opponent-policy likelihood is
inferred from their choice not to play a card.

A hidden VP outcome can make only some worlds terminal. Ongoing worlds remain
available until a public continuation excludes terminal worlds or
`ObserveVictory` conditions on an observed victory. The CLI/viewer never
reports a win just because one hypothetical hidden draw would have won.

This is a root-determinization implementation rather than full ISMCTS. A
worker's opponent decisions use the private hand in that determinization,
which is appropriate for the opponent's own knowledge, but information-set
nodes are not yet shared between worlds. The visualizer can independently
pause or resume either player's forest.

## Considered but not implemented

### Blanket pruning of Monopoly before rolling

Do **not** prune every pre-roll Monopoly. Rolling a seven can make playing it
first valuable as a discard buffer. For example, if the player holds ten
valuable resource cards and takes nine lumber with Monopoly, the ranked 1v1
discard rule makes them discard nine of nineteen cards after a seven. They can
discard the newly acquired lumber and preserve the original ten cards; without
Monopoly they would discard five valuable cards.

Waiting until after any non-seven roll is weakly better: no opponent acts in
between, and production can only leave the target holding unchanged or add to
it. Therefore a narrower prune when the upcoming probability of seven is
exactly zero appears safe, but it is not implemented because the gain is small
and the unconditional rule is easier to audit.

The acting player having fewer than ten resource cards is **not** a safe prune
condition. Suppose that player has no resources and the opponent has five
lumber plus fifteen other cards. On a seven, the opponent can discard all five
lumber before a post-roll Monopoly. Playing Lumber Monopoly first preserves
those five cards while leaving the acting player below the discard threshold.
It can also improve the subsequent robber steal: removing the target resource
first prevents the random steal from taking a card that Monopoly would have
collected anyway. Consequently any pre-roll pruning while seven remains
possible needs substantially stronger proof than a hand-size check.

### Global transposition table

A table keyed by complete `GameState` could merge multi-step action-order
transpositions. It is deferred because shared-node visit accounting across
different parents needs a deliberate MCTS design, especially once hidden
information is introduced.
