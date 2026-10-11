# Ranked 1v1 rule-test coverage

The suite treats Colonist's published rules as the executable specification:

- [Base-game rules](https://colonist.io/catan-rules)
- [Ranked 1v1 settings and Friendly Robber](https://blog.colonist.io/ranked-1v1-comprehensive-strategy-guide-colonist-io/)
- [Balanced seven explanation](https://blog.colonist.io/balancing-7s-on-1v1/)
- [Published balanced-dice implementation](https://pastebin.com/wWH4Ka96)
- [Colonist clarifications for development cards and bank shortages](https://blog.colonist.io/settling-the-rules-of-catan/)

## Covered

| Domain | Behaviour |
|---|---|
| Board | 19 hexes, 54 vertices, 72 edges, terrain/token/port inventories, desert robber, standard A-R number spirals in six orientations with the desert skipped, separated 6/8 tiles, deterministic seeds |
| Setup | P1-P2-P2-P1 order, distance rule, attached roads, resources from second settlement only, P1 first turn |
| Ranked | Two players, no domestic-trade action, 15-point win, discard only above 9, Friendly Robber based only on public points |
| Production | Settlement/city yields, number matching, robber blocking, bank conservation and shortages |
| Building | Published costs, connectivity, blocking, distance rule, ownership, piece limits |
| Maritime trade | 4:1 bank, 3:1 generic port, 2:1 matching port, bank availability |
| Robber | Both-player discards, exact discard selection, legal movement, weighted random steal |
| Development | Deck composition, buying/drawing, new-card delay, one per turn, all four action cards, hidden VP scoring |
| Awards | Longest-road trails/cycles/interruption/ties/transfers; largest-army threshold/ties/transfers |
| Balanced dice | 36-card deck, no replacement, 5-roll memory, 0.34 recent-roll reduction, reshuffle before the draw after 12 cards remain, 0.4 seven-streak adjustment, per-player seven balancing and clamping |
| Lifecycle | Phase legality, turn hand-off, resignation, rollback, invariants, chance-node normalization, deterministic playouts |
| Full game | Seeded setup-to-15-point system scenario using only legal actions; both-player discard, robber steals, building, maritime trade, every development-card type, both awards, and natural victory |

## Boundaries of “exact”

Colonist does not publish every server implementation detail. The tests therefore do **not** claim byte-for-byte parity for:

- the proprietary mapping from a seed to a particular randomized board;
- timers, disconnects, automatic choices on timeout, animations, or matchmaking;
- unannounced server-side changes made after the linked published rules/code.

Generated boards use shuffled terrain and standard A-R numbers in one of six
counterclockwise spirals, skipping the desert. This approximates Colonist's
normal Base map based on its published
[six orientations](https://colonist.io/patch-notes) and separate
[Base Random map](https://colonist.io/store); the exact spiral implementation,
orientation probabilities, and port distribution are not confirmed by Colonist.
Tests cover every desert position and orientation, standard inventories,
no-adjacent-6/8, and reproducible seeds. Imported boards retain their supplied
layouts and need not follow the generated spiral. Game-rule or balanced-dice
changes should update this document and the corresponding named tests together.
