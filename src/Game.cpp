#include "catan/Game.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>
#include <sstream>
#include <type_traits>

namespace catan {
namespace {

constexpr std::array<std::uint8_t, 13> StandardDiceCounts{0, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};
constexpr std::array<PlayerId, 4> PlacementOrder{PlayerId::One, PlayerId::Two, PlayerId::Two, PlayerId::One};

std::uint8_t countRoads(const GameState& state, PlayerId player) {
    return static_cast<std::uint8_t>(std::count_if(state.board.roads.begin(), state.board.roads.end(),
        [&](const auto& road) { return road && road->owner == player; }));
}

std::uint8_t countBuildings(const GameState& state, PlayerId player, BuildingType type) {
    return static_cast<std::uint8_t>(std::count_if(state.board.buildings.begin(), state.board.buildings.end(),
        [&](const auto& building) { return building && building->owner == player && building->type == type; }));
}

bool hasPlayable(const PlayerState& player, DevelopmentCard card) {
    const auto i = index(card);
    return player.developmentCards.cards[i] > player.developmentCards.boughtThisTurn[i];
}

std::uint16_t deckTotal(const DevelopmentDeckState& deck) {
    return std::accumulate(deck.cards.begin(), deck.cards.end(), std::uint16_t{0});
}

void enumerateDiscards(const Resources& hand, std::uint8_t needed, std::vector<Action>& actions) {
    Resources current{};
    std::function<void(std::size_t, unsigned)> recurse = [&](std::size_t resource, unsigned left) {
        if (resource == ResourceCount) {
            if (left == 0) actions.emplace_back(DiscardResources{current});
            return;
        }
        const unsigned maximum = std::min<unsigned>(hand[resource], left);
        for (unsigned amount = 0; amount <= maximum; ++amount) {
            current[resource] = static_cast<std::uint8_t>(amount);
            recurse(resource + 1, left - amount);
        }
        current[resource] = 0;
    };
    recurse(0, needed);
}

bool sameAction(const Action& left, const Action& right) {
    return left == right;
}

} // namespace

Game::Game(std::uint32_t boardSeed) {
    state_.board = makeStandardBoard(boardSeed);
    state_.dice.remainingBySum = StandardDiceCounts;
    state_.dice.cardsLeft = 36;
    state_.turn.phase = Phase::InitialSettlement;
    state_.turn.decisionPlayer = PlayerId::One;
}

Game::Game(GameState state) : state_(std::move(state)) {
    // Colonist reshuffles immediately before the next draw once the old deck
    // has fewer than 13 cards. Normalize an explicitly restored chance node
    // too, so loaded states follow the same rule as states reached via BeginRoll.
    if (state_.turn.phase == Phase::DiceRoll) prepareDiceDraw();
}

std::vector<Action> Game::legalActions() const {
    std::vector<Action> result;
    if (state_.turn.phase == Phase::Finished) return result;
    const PlayerId player = state_.turn.decisionPlayer;
    const auto& playerState = state_.players[index(player)];
    const auto& topology = BoardTopology::standard();

    switch (state_.turn.phase) {
    case Phase::InitialSettlement:
        for (std::size_t v = 0; v < BoardTopology::VertexCount; ++v) {
            VertexId vertex{static_cast<std::uint8_t>(v)};
            if (canBuildSettlement(state_, player, vertex, true)) result.emplace_back(PlaceSettlement{vertex});
        }
        break;
    case Phase::InitialRoad:
        if (state_.turn.pendingInitialSettlement) {
            const VertexId vertex = *state_.turn.pendingInitialSettlement;
            for (const auto edge : topology.vertices[vertex.value].edges)
                if (edge && !state_.board.roads[edge->value]) result.emplace_back(PlaceRoad{*edge});
        }
        break;
    case Phase::AwaitRoll:
        result.emplace_back(BeginRoll{});
        break;
    case Phase::DiceRoll:
        for (const auto& outcome : chanceOutcomes()) result.push_back(outcome.action);
        break;
    case Phase::Discarding:
        enumerateDiscards(playerState.resources, state_.turn.pendingDiscards[index(player)], result);
        break;
    case Phase::MoveRobber:
        for (std::size_t h = 0; h < BoardTopology::HexCount; ++h) {
            HexId hex{static_cast<std::uint8_t>(h)};
            if (robberMayTarget(state_, player, hex)) result.emplace_back(MoveRobber{hex});
        }
        break;
    case Phase::RobberSteal: {
        const auto& victim = state_.players[index(opponent(player))];
        for (std::size_t r = 0; r < ResourceCount; ++r)
            if (victim.resources[r]) result.emplace_back(StealResource{static_cast<Resource>(r)});
        break;
    }
    case Phase::Main:
        if (canAfford(playerState, RoadCost) && countRoads(state_, player) < 15)
            for (std::size_t e = 0; e < BoardTopology::EdgeCount; ++e) {
                EdgeId edge{static_cast<std::uint8_t>(e)};
                if (canBuildRoad(state_, player, edge)) result.emplace_back(BuildRoad{edge});
            }
        if (canAfford(playerState, SettlementCost) && countBuildings(state_, player, BuildingType::Settlement) < 5)
            for (std::size_t v = 0; v < BoardTopology::VertexCount; ++v) {
                VertexId vertex{static_cast<std::uint8_t>(v)};
                if (canBuildSettlement(state_, player, vertex, false)) result.emplace_back(BuildSettlement{vertex});
            }
        if (canAfford(playerState, CityCost) && countBuildings(state_, player, BuildingType::City) < 4)
            for (std::size_t v = 0; v < BoardTopology::VertexCount; ++v) {
                VertexId vertex{static_cast<std::uint8_t>(v)};
                if (canBuildCity(state_, player, vertex)) result.emplace_back(BuildCity{vertex});
            }
        if (canAfford(playerState, DevelopmentCost) && deckTotal(state_.developmentDeck) > 0)
            result.emplace_back(BuyDevelopmentCard{});
        for (std::size_t give = 0; give < ResourceCount; ++give) {
            const auto resource = static_cast<Resource>(give);
            const auto rate = maritimeRate(state_, player, resource);
            if (playerState.resources[give] < rate) continue;
            for (std::size_t receive = 0; receive < ResourceCount; ++receive)
                if (receive != give && state_.bank.cards[receive])
                    result.emplace_back(MaritimeTrade{resource, static_cast<Resource>(receive)});
        }
        result.emplace_back(EndTurn{});
        break;
    case Phase::DevelopmentDraw:
        for (std::size_t card = 0; card < DevelopmentCardTypeCount; ++card)
            if (state_.developmentDeck.cards[card]) result.emplace_back(DrawDevelopmentCard{static_cast<DevelopmentCard>(card)});
        break;
    case Phase::FreeRoads:
        for (std::size_t e = 0; e < BoardTopology::EdgeCount; ++e) {
            EdgeId edge{static_cast<std::uint8_t>(e)};
            if (canBuildRoad(state_, player, edge)) result.emplace_back(PlaceRoad{edge});
        }
        break;
    case Phase::YearOfPlenty:
        for (std::size_t r = 0; r < ResourceCount; ++r)
            if (state_.bank.cards[r]) result.emplace_back(ChooseYearOfPlentyResource{static_cast<Resource>(r)});
        break;
    case Phase::Finished:
        break;
    }

    const bool developmentWindow = state_.turn.phase == Phase::AwaitRoll || state_.turn.phase == Phase::Main;
    if (developmentWindow && !state_.turn.playedDevelopmentCard) {
        if (hasPlayable(playerState, DevelopmentCard::Knight)) result.emplace_back(PlayKnight{});
        if (hasPlayable(playerState, DevelopmentCard::RoadBuilding) && countRoads(state_, player) < 15) {
            bool any = false;
            for (std::size_t e = 0; e < BoardTopology::EdgeCount; ++e)
                any |= canBuildRoad(state_, player, EdgeId{static_cast<std::uint8_t>(e)});
            if (any) result.emplace_back(PlayRoadBuilding{});
        }
        if (hasPlayable(playerState, DevelopmentCard::YearOfPlenty) && total(state_.bank.cards) >= 2)
            result.emplace_back(PlayYearOfPlenty{});
        if (hasPlayable(playerState, DevelopmentCard::Monopoly))
            for (std::size_t r = 0; r < ResourceCount; ++r)
                result.emplace_back(PlayMonopoly{static_cast<Resource>(r)});
    }

    const bool playerDecision = state_.turn.phase != Phase::DevelopmentDraw &&
                                state_.turn.phase != Phase::RobberSteal &&
                                state_.turn.phase != Phase::DiceRoll;
    if (playerDecision) result.emplace_back(Resign{});
    return result;
}

std::vector<WeightedAction> Game::chanceOutcomes() const {
    std::vector<WeightedAction> outcomes;
    if (state_.turn.phase == Phase::DiceRoll) {
        std::array<double, 13> weights{};
        double weightTotal = 0.0;
        for (std::size_t sum = 2; sum <= 12; ++sum) {
            double weight = static_cast<double>(state_.dice.remainingBySum[sum]) / state_.dice.cardsLeft;
            weight *= std::max(0.0, 1.0 - state_.dice.recentCounts[sum] * 0.34);
            if (sum == 7) {
                const auto player = state_.turn.decisionPlayer;
                const unsigned totalSevens = state_.dice.sevensByPlayer[0] + state_.dice.sevensByPlayer[1];
                double balance = 1.0;
                if (totalSevens >= 2) {
                    const double share = static_cast<double>(state_.dice.sevensByPlayer[index(player)]) / totalSevens;
                    balance = 2.0 - 2.0 * share;
                }
                double streak = 0.0;
                if (state_.dice.sevenStreakPlayer)
                    streak = 0.4 * state_.dice.sevenStreakCount *
                        (*state_.dice.sevenStreakPlayer == player ? -1.0 : 1.0);
                weight *= std::clamp(balance + streak, 0.0, 2.0);
            }
            weights[sum] = weight;
            weightTotal += weight;
        }
        for (std::uint8_t sum = 2; sum <= 12; ++sum)
            if (weights[sum] > 0) outcomes.push_back({RollDice{sum}, weights[sum] / weightTotal});
    } else if (state_.turn.phase == Phase::DevelopmentDraw) {
        const double cards = deckTotal(state_.developmentDeck);
        for (std::size_t card = 0; card < DevelopmentCardTypeCount; ++card)
            if (state_.developmentDeck.cards[card])
                outcomes.push_back({DrawDevelopmentCard{static_cast<DevelopmentCard>(card)},
                                    state_.developmentDeck.cards[card] / cards});
    } else if (state_.turn.phase == Phase::RobberSteal) {
        const auto& victim = state_.players[index(opponent(state_.turn.decisionPlayer))];
        const double cards = total(victim.resources);
        for (std::size_t resource = 0; resource < ResourceCount; ++resource)
            if (victim.resources[resource])
                outcomes.push_back({StealResource{static_cast<Resource>(resource)}, victim.resources[resource] / cards});
    }
    return outcomes;
}

void Game::pay(PlayerId player, const Resources& cost) {
    for (std::size_t r = 0; r < ResourceCount; ++r) {
        state_.players[index(player)].resources[r] -= cost[r];
        state_.bank.cards[r] += cost[r];
    }
}

void Game::grantInitialResources(VertexId vertex, PlayerId player, std::vector<std::string>& events) {
    const auto& topology = BoardTopology::standard();
    for (const auto hex : topology.vertices[vertex.value].hexes) {
        if (!hex) continue;
        const auto resource = producedResource(state_.board.hexes[hex->value].terrain);
        if (resource && state_.bank.cards[index(*resource)]) {
            --state_.bank.cards[index(*resource)];
            ++state_.players[index(player)].resources[index(*resource)];
        }
    }
    events.push_back(std::string(toString(player)) + " receives starting resources");
}

void Game::distribute(std::uint8_t roll, std::vector<std::string>& events) {
    std::array<Resources, PlayerCount> owed{};
    const auto& topology = BoardTopology::standard();
    for (std::size_t h = 0; h < BoardTopology::HexCount; ++h) {
        const auto& hex = state_.board.hexes[h];
        if (hex.number != roll || state_.board.robberHex.value == h) continue;
        const auto resource = producedResource(hex.terrain);
        if (!resource) continue;
        for (VertexId vertex : topology.hexes[h].vertices) {
            const auto& building = state_.board.buildings[vertex.value];
            if (!building) continue;
            owed[index(building->owner)][index(*resource)] +=
                building->type == BuildingType::City ? 2 : 1;
        }
    }
    for (std::size_t r = 0; r < ResourceCount; ++r) {
        const unsigned required = owed[0][r] + owed[1][r];
        if (required > state_.bank.cards[r]) {
            const unsigned claimants = (owed[0][r] > 0 ? 1U : 0U) + (owed[1][r] > 0 ? 1U : 0U);
            if (claimants == 1) {
                const std::size_t player = owed[0][r] ? 0 : 1;
                const auto available = state_.bank.cards[r];
                state_.players[player].resources[r] += available;
                state_.bank.cards[r] = 0;
                events.push_back(std::string("bank shortage: only ") + std::to_string(available) + ' ' +
                                 toString(static_cast<Resource>(r)) + " distributed");
            } else if (required) {
                events.push_back(std::string("bank shortage: no ") +
                                 toString(static_cast<Resource>(r)) + " distributed");
            }
            continue;
        }
        for (std::size_t p = 0; p < PlayerCount; ++p) {
            state_.players[p].resources[r] += owed[p][r];
            state_.bank.cards[r] -= owed[p][r];
        }
    }
    events.push_back("resources distributed for roll " + std::to_string(roll));
}

void Game::prepareDiceDraw() {
    auto& dice = state_.dice;
    if (dice.cardsLeft >= 13) return;
    dice.remainingBySum = StandardDiceCounts;
    dice.cardsLeft = 36;
}

void Game::updateDice(std::uint8_t roll) {
    auto& dice = state_.dice;
    --dice.remainingBySum[roll];
    --dice.cardsLeft;
    if (dice.recentRollCount == dice.recentRolls.size()) {
        --dice.recentCounts[dice.recentRolls[0]];
        std::move(dice.recentRolls.begin() + 1, dice.recentRolls.end(), dice.recentRolls.begin());
        --dice.recentRollCount;
    }
    dice.recentRolls[dice.recentRollCount++] = roll;
    ++dice.recentCounts[roll];
    if (roll == 7) {
        ++dice.sevensByPlayer[index(state_.activePlayer)];
        if (dice.sevenStreakPlayer == state_.activePlayer) ++dice.sevenStreakCount;
        else { dice.sevenStreakPlayer = state_.activePlayer; dice.sevenStreakCount = 1; }
    }
}

void Game::updateAwards() {
    for (std::size_t p = 0; p < PlayerCount; ++p)
        state_.awards.roadLengths[p] = longestRoad(state_, static_cast<PlayerId>(p));
    const auto road0 = state_.awards.roadLengths[0], road1 = state_.awards.roadLengths[1];
    if (state_.awards.longestRoad) {
        const auto holder = *state_.awards.longestRoad;
        const auto other = opponent(holder);
        if (state_.awards.roadLengths[index(holder)] < 5) {
            state_.awards.longestRoad = state_.awards.roadLengths[index(other)] >= 5
                ? std::optional<PlayerId>{other} : std::nullopt;
        } else if (state_.awards.roadLengths[index(other)] >= 5 &&
            state_.awards.roadLengths[index(other)] > state_.awards.roadLengths[index(holder)])
            state_.awards.longestRoad = other;
    } else if (road0 >= 5 && road0 > road1) state_.awards.longestRoad = PlayerId::One;
    else if (road1 >= 5 && road1 > road0) state_.awards.longestRoad = PlayerId::Two;

    const auto army0 = state_.players[0].playedKnights, army1 = state_.players[1].playedKnights;
    if (state_.awards.largestArmy) {
        const auto holder = *state_.awards.largestArmy;
        const auto other = opponent(holder);
        if (state_.players[index(other)].playedKnights >= 3 &&
            state_.players[index(other)].playedKnights > state_.players[index(holder)].playedKnights)
            state_.awards.largestArmy = other;
    } else if (army0 >= 3 && army0 > army1) state_.awards.largestArmy = PlayerId::One;
    else if (army1 >= 3 && army1 > army0) state_.awards.largestArmy = PlayerId::Two;
}

void Game::checkWinner(PlayerId player, std::vector<std::string>& events) {
    if (victoryPoints(state_, player) < 15) return;
    state_.winner = player;
    state_.turn.phase = Phase::Finished;
    events.push_back(std::string(toString(player)) + " wins with " + std::to_string(victoryPoints(state_, player)) + " points");
}

void Game::finishDevelopmentResolution() { state_.turn.phase = state_.turn.returnPhase; }

void Game::finishTurn(std::vector<std::string>& events) {
    auto& hand = state_.players[index(state_.activePlayer)].developmentCards;
    hand.boughtThisTurn.fill(0);
    state_.activePlayer = opponent(state_.activePlayer);
    state_.turn.decisionPlayer = state_.activePlayer;
    state_.turn.phase = Phase::AwaitRoll;
    state_.turn.playedDevelopmentCard = false;
    ++state_.turnNumber;
    events.push_back(std::string("turn passes to ") + toString(state_.activePlayer));
}

void Game::beginRobber(PlayerId player, Phase returnPhase) {
    state_.turn.decisionPlayer = player;
    state_.turn.returnPhase = returnPhase;
    state_.turn.phase = Phase::MoveRobber;
}

void Game::finishRobberMove(std::vector<std::string>& events) {
    const PlayerId player = state_.turn.decisionPlayer;
    const PlayerId victim = opponent(player);
    bool victimAdjacent = false;
    const auto& topology = BoardTopology::standard();
    for (VertexId vertex : topology.hexes[state_.board.robberHex.value].vertices) {
        const auto& building = state_.board.buildings[vertex.value];
        victimAdjacent |= building && building->owner == victim;
    }
    if (victimAdjacent && total(state_.players[index(victim)].resources) > 0) {
        state_.turn.phase = Phase::RobberSteal;
    } else {
        finishDevelopmentResolution();
        events.push_back("no resource can be stolen");
    }
}

void Game::autoFinishFreeRoads() {
    if (state_.turn.phase != Phase::FreeRoads) return;
    bool canPlace = state_.turn.freeRoadsRemaining && countRoads(state_, state_.turn.decisionPlayer) < 15;
    if (canPlace) {
        canPlace = false;
        for (std::size_t e = 0; e < BoardTopology::EdgeCount; ++e)
            canPlace |= canBuildRoad(state_, state_.turn.decisionPlayer, EdgeId{static_cast<std::uint8_t>(e)});
    }
    if (!canPlace) finishDevelopmentResolution();
}

StepResult Game::step(const Action& action) {
    const auto legal = legalActions();
    if (std::none_of(legal.begin(), legal.end(), [&](const Action& candidate) { return sameAction(candidate, action); }))
        return {false, "illegal action in phase " + std::string(toString(state_.turn.phase)), {}};

    const GameState before = state_;
    StepResult result{true, "ok", {toString(action)}};
    const PlayerId player = state_.turn.decisionPlayer;

    if (const auto* value = std::get_if<PlaceSettlement>(&action)) {
        state_.board.buildings[value->vertex.value] = Building{player, BuildingType::Settlement};
        state_.turn.pendingInitialSettlement = value->vertex;
        state_.turn.phase = Phase::InitialRoad;
    } else if (const auto* value = std::get_if<PlaceRoad>(&action)) {
        state_.board.roads[value->edge.value] = Road{player};
        if (state_.turn.phase == Phase::InitialRoad) {
            if (state_.turn.placementIndex >= 2) grantInitialResources(*state_.turn.pendingInitialSettlement, player, result.events);
            state_.turn.pendingInitialSettlement.reset();
            ++state_.turn.placementIndex;
            if (state_.turn.placementIndex == PlacementOrder.size()) {
                state_.activePlayer = PlayerId::One;
                state_.turn.decisionPlayer = PlayerId::One;
                state_.turn.phase = Phase::AwaitRoll;
                state_.turnNumber = 1;
            } else {
                state_.turn.decisionPlayer = PlacementOrder[state_.turn.placementIndex];
                state_.turn.phase = Phase::InitialSettlement;
            }
        } else {
            --state_.turn.freeRoadsRemaining;
            updateAwards();
            checkWinner(player, result.events);
            if (!state_.winner) autoFinishFreeRoads();
        }
    } else if (std::holds_alternative<BeginRoll>(action)) {
        prepareDiceDraw();
        state_.turn.phase = Phase::DiceRoll;
    } else if (const auto* value = std::get_if<RollDice>(&action)) {
        updateDice(value->sum);
        if (value->sum == 7) {
            for (std::size_t p = 0; p < PlayerCount; ++p) {
                const auto cards = total(state_.players[p].resources);
                state_.turn.pendingDiscards[p] = cards > 9 ? static_cast<std::uint8_t>(cards / 2) : 0;
            }
            auto first = std::find_if(state_.turn.pendingDiscards.begin(), state_.turn.pendingDiscards.end(), [](auto n) { return n > 0; });
            if (first != state_.turn.pendingDiscards.end()) {
                state_.turn.phase = Phase::Discarding;
                state_.turn.decisionPlayer = static_cast<PlayerId>(std::distance(state_.turn.pendingDiscards.begin(), first));
                state_.turn.returnPhase = Phase::Main;
            } else beginRobber(state_.activePlayer, Phase::Main);
        } else {
            distribute(value->sum, result.events);
            state_.turn.phase = Phase::Main;
        }
    } else if (const auto* value = std::get_if<DiscardResources>(&action)) {
        for (std::size_t r = 0; r < ResourceCount; ++r) {
            state_.players[index(player)].resources[r] -= value->cards[r];
            state_.bank.cards[r] += value->cards[r];
        }
        state_.turn.pendingDiscards[index(player)] = 0;
        auto next = std::find_if(state_.turn.pendingDiscards.begin(), state_.turn.pendingDiscards.end(), [](auto n) { return n > 0; });
        if (next == state_.turn.pendingDiscards.end()) beginRobber(state_.activePlayer, Phase::Main);
        else state_.turn.decisionPlayer = static_cast<PlayerId>(std::distance(state_.turn.pendingDiscards.begin(), next));
    } else if (const auto* value = std::get_if<MoveRobber>(&action)) {
        state_.board.robberHex = value->hex;
        finishRobberMove(result.events);
        if (state_.turn.phase != Phase::RobberSteal) checkWinner(player, result.events);
    } else if (const auto* value = std::get_if<StealResource>(&action)) {
        const PlayerId victim = opponent(player);
        --state_.players[index(victim)].resources[index(value->resource)];
        ++state_.players[index(player)].resources[index(value->resource)];
        finishDevelopmentResolution();
        checkWinner(player, result.events);
    } else if (const auto* value = std::get_if<BuildRoad>(&action)) {
        pay(player, RoadCost);
        state_.board.roads[value->edge.value] = Road{player};
        updateAwards();
        checkWinner(player, result.events);
    } else if (const auto* value = std::get_if<BuildSettlement>(&action)) {
        pay(player, SettlementCost);
        state_.board.buildings[value->vertex.value] = Building{player, BuildingType::Settlement};
        updateAwards();
        checkWinner(player, result.events);
    } else if (const auto* value = std::get_if<BuildCity>(&action)) {
        pay(player, CityCost);
        state_.board.buildings[value->vertex.value]->type = BuildingType::City;
        checkWinner(player, result.events);
    } else if (const auto* value = std::get_if<MaritimeTrade>(&action)) {
        const auto rate = maritimeRate(state_, player, value->give);
        state_.players[index(player)].resources[index(value->give)] -= rate;
        state_.bank.cards[index(value->give)] += rate;
        --state_.bank.cards[index(value->receive)];
        ++state_.players[index(player)].resources[index(value->receive)];
    } else if (std::holds_alternative<BuyDevelopmentCard>(action)) {
        pay(player, DevelopmentCost);
        state_.turn.phase = Phase::DevelopmentDraw;
    } else if (const auto* value = std::get_if<DrawDevelopmentCard>(&action)) {
        --state_.developmentDeck.cards[index(value->card)];
        ++state_.players[index(player)].developmentCards.cards[index(value->card)];
        ++state_.players[index(player)].developmentCards.boughtThisTurn[index(value->card)];
        state_.turn.phase = Phase::Main;
        checkWinner(player, result.events);
    } else if (std::holds_alternative<PlayKnight>(action)) {
        --state_.players[index(player)].developmentCards.cards[index(DevelopmentCard::Knight)];
        ++state_.players[index(player)].playedKnights;
        state_.turn.playedDevelopmentCard = true;
        state_.turn.returnPhase = state_.turn.phase;
        updateAwards();
        beginRobber(player, state_.turn.returnPhase);
    } else if (std::holds_alternative<PlayRoadBuilding>(action)) {
        --state_.players[index(player)].developmentCards.cards[index(DevelopmentCard::RoadBuilding)];
        state_.turn.playedDevelopmentCard = true;
        state_.turn.returnPhase = state_.turn.phase;
        state_.turn.phase = Phase::FreeRoads;
        state_.turn.freeRoadsRemaining = 2;
        autoFinishFreeRoads();
    } else if (std::holds_alternative<PlayYearOfPlenty>(action)) {
        --state_.players[index(player)].developmentCards.cards[index(DevelopmentCard::YearOfPlenty)];
        state_.turn.playedDevelopmentCard = true;
        state_.turn.returnPhase = state_.turn.phase;
        state_.turn.phase = Phase::YearOfPlenty;
        state_.turn.plentyPicksRemaining = 2;
    } else if (const auto* value = std::get_if<ChooseYearOfPlentyResource>(&action)) {
        --state_.bank.cards[index(value->resource)];
        ++state_.players[index(player)].resources[index(value->resource)];
        if (--state_.turn.plentyPicksRemaining == 0) finishDevelopmentResolution();
    } else if (const auto* value = std::get_if<PlayMonopoly>(&action)) {
        --state_.players[index(player)].developmentCards.cards[index(DevelopmentCard::Monopoly)];
        state_.turn.playedDevelopmentCard = true;
        const PlayerId victim = opponent(player);
        const auto amount = state_.players[index(victim)].resources[index(value->resource)];
        state_.players[index(victim)].resources[index(value->resource)] = 0;
        state_.players[index(player)].resources[index(value->resource)] += amount;
    } else if (std::holds_alternative<EndTurn>(action)) {
        finishTurn(result.events);
    } else if (std::holds_alternative<Resign>(action)) {
        state_.winner = opponent(player);
        state_.turn.phase = Phase::Finished;
        result.events.push_back(std::string(toString(player)) + " resigned");
    }

    std::string invalidReason;
    if (!validateState(state_, &invalidReason)) {
        state_ = before;
        result.accepted = false;
        result.message = "internal state invariant failed: " + invalidReason;
    }
    return result;
}

} // namespace catan
