#include "catan/Game.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

using namespace catan;

namespace {

constexpr std::uint32_t BoardSeed = 20260927;

[[noreturn]] void fail(const std::string& message) {
    throw std::runtime_error(message);
}

template <typename T>
std::vector<T> actions(const Game& game) {
    std::vector<T> result;
    for (const auto& action : game.legalActions())
        if (const auto* value = std::get_if<T>(&action)) result.push_back(*value);
    return result;
}

template <typename T>
std::optional<T> firstAction(const Game& game) {
    for (const auto& action : game.legalActions())
        if (const auto* value = std::get_if<T>(&action)) return *value;
    return std::nullopt;
}

unsigned buildingCount(const GameState& state, PlayerId player,
                       std::optional<BuildingType> type = std::nullopt) {
    return static_cast<unsigned>(std::count_if(state.board.buildings.begin(), state.board.buildings.end(),
        [&](const auto& building) {
            return building && building->owner == player && (!type || building->type == *type);
        }));
}

unsigned roadCount(const GameState& state, PlayerId player) {
    return static_cast<unsigned>(std::count_if(state.board.roads.begin(), state.board.roads.end(),
        [&](const auto& road) { return road && road->owner == player; }));
}

unsigned pipWeight(std::uint8_t number) {
    if (number < 2 || number > 12 || number == 7) return 0;
    return 6U - static_cast<unsigned>(number > 7 ? number - 7 : 7 - number);
}

unsigned vertexScore(const GameState& state, VertexId vertex) {
    unsigned score = 0;
    std::array<bool, ResourceCount> resources{};
    const auto& topology = BoardTopology::standard();
    for (const auto hex : topology.vertices[vertex.value].hexes) {
        if (!hex) continue;
        const auto& tile = state.board.hexes[hex->value];
        score += pipWeight(tile.number) * 10;
        if (const auto resource = producedResource(tile.terrain)) resources[index(*resource)] = true;
    }
    score += static_cast<unsigned>(std::count(resources.begin(), resources.end(), true)) * 7;
    for (const auto& port : state.board.ports) {
        const auto ends = topology.edges[port.edge.value].vertices;
        if (ends[0] == vertex || ends[1] == vertex) score += 4;
    }
    return score;
}

bool distanceRuleClear(const GameState& state, VertexId vertex) {
    if (state.board.buildings[vertex.value]) return false;
    for (const auto neighbour : BoardTopology::standard().vertices[vertex.value].neighbours)
        if (neighbour && state.board.buildings[neighbour->value]) return false;
    return true;
}

class FullGameScenario {
public:
    FullGameScenario() : game_(BoardSeed) {}

    void run() {
        rejectAnIllegalOpeningAction();
        performInitialPlacement();
        createAndResolveASevenDiscard();

        while (game_.state().turn.phase != Phase::Finished && turnsPlayed_ < 400) playTurn();

        require(game_.state().turn.phase == Phase::Finished, "game did not finish within 400 turns");
        require(game_.state().winner == std::optional<PlayerId>{PlayerId::One},
                "the scripted player did not win");
        require(victoryPoints(game_.state(), PlayerId::One) >= 15, "winner has fewer than 15 points");
        require(!resigned_, "the game ended by resignation instead of normal victory");
        require(discardDecisions_ >= 2, "the seven did not make both players discard");
        require(robberSteals_ >= 1, "no robber steal occurred");
        require(freeRoadsPlaced_ == 2, "Road Building did not place two roads");
        require(plentyPicks_ == 2, "Year of Plenty did not take two resources");
        require(monopoliesPlayed_ == 1, "Monopoly was not played");
        require(game_.state().players[index(PlayerId::One)].playedKnights >= 3,
                "three knights were not played");
        require(game_.state().awards.largestArmy == std::optional<PlayerId>{PlayerId::One},
                "Largest Army was not earned");
        require(game_.state().awards.longestRoad == std::optional<PlayerId>{PlayerId::One},
                "Longest Road was not earned");
        require(maritimeTrades_ >= 1, "no maritime trade occurred");

        for (std::size_t action = 0; action < ResignIndex; ++action)
            require(seenActions_[action], "an expected action variant was never exercised: index " +
                                         std::to_string(action));

        std::string reason;
        require(validateState(game_.state(), &reason), "final state invariant failed: " + reason);
    }

    void report() const {
        std::cout << "full ranked game completed in " << turnsPlayed_ << " turns and "
                  << acceptedSteps_ << " accepted steps\n"
                  << "winner: Player 1 with " << static_cast<unsigned>(victoryPoints(game_.state(), PlayerId::One))
                  << " points; roads=" << roadCount(game_.state(), PlayerId::One)
                  << ", settlements=" << buildingCount(game_.state(), PlayerId::One, BuildingType::Settlement)
                  << ", cities=" << buildingCount(game_.state(), PlayerId::One, BuildingType::City)
                  << ", trades=" << maritimeTrades_ << ", robber steals=" << robberSteals_ << '\n';
    }

private:
    static constexpr std::size_t ResignIndex = std::variant_size_v<Action> - 1;
    Game game_;
    std::array<bool, std::variant_size_v<Action>> seenActions_{};
    std::vector<std::string> trace_;
    std::size_t nextDevelopmentCard_{};
    unsigned acceptedSteps_{};
    unsigned turnsPlayed_{};
    unsigned discardDecisions_{};
    unsigned robberSteals_{};
    unsigned freeRoadsPlaced_{};
    unsigned plentyPicks_{};
    unsigned monopoliesPlayed_{};
    unsigned maritimeTrades_{};
    bool resigned_{};

    static constexpr std::array<DevelopmentCard, 8> DevelopmentPlan{
        DevelopmentCard::RoadBuilding,
        DevelopmentCard::YearOfPlenty,
        DevelopmentCard::Monopoly,
        DevelopmentCard::Knight,
        DevelopmentCard::Knight,
        DevelopmentCard::Knight,
        DevelopmentCard::VictoryPoint,
        DevelopmentCard::VictoryPoint,
    };

    void require(bool condition, const std::string& message) const {
        if (condition) return;
        std::ostringstream detail;
        detail << message << " [turn=" << game_.state().turnNumber
               << ", phase=" << toString(game_.state().turn.phase)
               << ", active=" << toString(game_.state().activePlayer) << ']';
        if (!trace_.empty()) detail << " last action=" << trace_.back();
        fail(detail.str());
    }

    void apply(const Action& action) {
        const auto phase = game_.state().turn.phase;
        const auto result = game_.step(action);
        require(result.accepted, "legal scenario action was rejected: " + toString(action) + ": " + result.message);
        seenActions_[action.index()] = true;
        ++acceptedSteps_;
        trace_.push_back(toString(action));
        if (std::holds_alternative<DiscardResources>(action)) ++discardDecisions_;
        if (std::holds_alternative<StealResource>(action)) ++robberSteals_;
        if (std::holds_alternative<ChooseYearOfPlentyResource>(action)) ++plentyPicks_;
        if (std::holds_alternative<PlayMonopoly>(action)) ++monopoliesPlayed_;
        if (std::holds_alternative<MaritimeTrade>(action)) ++maritimeTrades_;
        if (std::holds_alternative<Resign>(action)) resigned_ = true;
        if (phase == Phase::FreeRoads && std::holds_alternative<PlaceRoad>(action)) ++freeRoadsPlaced_;

        std::string reason;
        require(validateState(game_.state(), &reason), "state invariant failed after " + toString(action) + ": " + reason);
    }

    void rejectAnIllegalOpeningAction() {
        const auto before = game_.state().turn.phase;
        const auto result = game_.step(BeginRoll{});
        require(!result.accepted, "BeginRoll should be rejected during initial placement");
        require(game_.state().turn.phase == before, "rejected action changed the phase");
    }

    PlaceSettlement bestSettlement() const {
        const auto choices = actions<PlaceSettlement>(game_);
        require(!choices.empty(), "no legal initial settlement");
        return *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
            return vertexScore(game_.state(), left.vertex) < vertexScore(game_.state(), right.vertex);
        });
    }

    template <typename RoadAction>
    RoadAction bestRoad() const {
        const auto choices = actions<RoadAction>(game_);
        require(!choices.empty(), "no legal road action");
        const auto& topology = BoardTopology::standard();
        return *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
            auto score = [&](EdgeId edge) {
                unsigned value = 0;
                auto trial = game_.state();
                trial.board.roads[edge.value] = Road{game_.decisionPlayer()};
                value += static_cast<unsigned>(longestRoad(trial, game_.decisionPlayer())) * 1000;
                for (const auto vertex : topology.edges[edge.value].vertices) {
                    value += vertexScore(game_.state(), vertex);
                    if (distanceRuleClear(game_.state(), vertex)) value += 250;
                    for (const auto neighbour : topology.vertices[vertex.value].neighbours)
                        if (neighbour && distanceRuleClear(game_.state(), *neighbour)) value += 40;
                }
                return value;
            };
            return score(left.edge) < score(right.edge);
        });
    }

    void performInitialPlacement() {
        while (game_.state().turn.phase == Phase::InitialSettlement ||
               game_.state().turn.phase == Phase::InitialRoad) {
            if (game_.state().turn.phase == Phase::InitialSettlement) apply(bestSettlement());
            else apply(bestRoad<PlaceRoad>());
        }
        require(game_.state().turn.phase == Phase::AwaitRoll, "setup did not enter AwaitRoll");
        require(buildingCount(game_.state(), PlayerId::One) == 2, "Player 1 setup building count is wrong");
        require(buildingCount(game_.state(), PlayerId::Two) == 2, "Player 2 setup building count is wrong");
    }

    Resources production(std::uint8_t roll, PlayerId player) const {
        Resources result{};
        const auto& state = game_.state();
        const auto& topology = BoardTopology::standard();
        for (std::size_t h = 0; h < BoardTopology::HexCount; ++h) {
            const auto& tile = state.board.hexes[h];
            if (tile.number != roll || state.board.robberHex.value == h) continue;
            const auto resource = producedResource(tile.terrain);
            if (!resource) continue;
            for (const auto vertex : topology.hexes[h].vertices) {
                const auto& building = state.board.buildings[vertex.value];
                if (building && building->owner == player)
                    result[index(*resource)] += building->type == BuildingType::City ? 2 : 1;
            }
        }
        return result;
    }

    RollDice bestProductionRoll(const Resources& target) const {
        const auto choices = actions<RollDice>(game_);
        require(!choices.empty(), "dice chance node has no outcomes");
        const auto& hand = game_.state().players[index(PlayerId::One)].resources;
        return *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
            auto score = [&](std::uint8_t sum) {
                if (sum == 7) return -100000;
                const auto one = production(sum, PlayerId::One);
                const auto two = production(sum, PlayerId::Two);
                int value = 0;
                for (std::size_t r = 0; r < ResourceCount; ++r) {
                    const bool needed = hand[r] < target[r];
                    value += one[r] * (needed ? 120 : 18);
                    value += two[r] * 5;
                }
                return value;
            };
            return score(left.sum) < score(right.sum);
        });
    }

    void beginAndRoll(const Resources& target) {
        apply(BeginRoll{});
        apply(bestProductionRoll(target));
        resolveForcedPhases();
    }

    void createAndResolveASevenDiscard() {
        const Resources accumulate{9, 9, 9, 9, 9};
        unsigned accumulationTurns = 0;
        while ((total(game_.state().players[0].resources) <= 9 ||
                total(game_.state().players[1].resources) <= 9) && accumulationTurns++ < 80) {
            beginAndRoll(accumulate);
            apply(EndTurn{});
            ++turnsPlayed_;
        }
        require(total(game_.state().players[0].resources) > 9, "Player 1 did not accumulate a discardable hand");
        require(total(game_.state().players[1].resources) > 9, "Player 2 did not accumulate a discardable hand");
        apply(BeginRoll{});
        const auto rolls = actions<RollDice>(game_);
        const auto seven = std::find_if(rolls.begin(), rolls.end(),
            [](const auto& roll) { return roll.sum == 7; });
        require(seven != rolls.end(), "balanced dice did not offer seven for discard scenario");
        apply(RollDice{7});
        resolveForcedPhases();
        require(game_.state().turn.phase == Phase::Main, "seven did not return to Main");
        apply(EndTurn{});
        ++turnsPlayed_;
    }

    HexId bestRobberDestination() const {
        const auto choices = actions<MoveRobber>(game_);
        require(!choices.empty(), "robber has no legal destination");
        const PlayerId mover = game_.decisionPlayer();
        const PlayerId victim = opponent(mover);
        const auto& topology = BoardTopology::standard();
        for (const auto choice : choices) {
            for (const auto vertex : topology.hexes[choice.hex.value].vertices) {
                const auto& building = game_.state().board.buildings[vertex.value];
                if (building && building->owner == victim &&
                    total(game_.state().players[index(victim)].resources) > 0) return choice.hex;
            }
        }
        return choices.front().hex;
    }

    void resolveForcedPhases() {
        while (true) {
            switch (game_.state().turn.phase) {
            case Phase::Discarding: {
                const auto discard = firstAction<DiscardResources>(game_);
                require(discard.has_value(), "discard phase has no legal discard");
                apply(*discard);
                break;
            }
            case Phase::MoveRobber:
                apply(MoveRobber{bestRobberDestination()});
                break;
            case Phase::RobberSteal: {
                const PlayerId victim = opponent(game_.decisionPlayer());
                const auto choices = actions<StealResource>(game_);
                require(!choices.empty(), "robber steal has no chance outcome");
                const auto best = *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
                    return game_.state().players[index(victim)].resources[index(left.resource)] <
                           game_.state().players[index(victim)].resources[index(right.resource)];
                });
                apply(best);
                break;
            }
            case Phase::FreeRoads:
                apply(bestRoad<PlaceRoad>());
                break;
            case Phase::YearOfPlenty: {
                const auto choices = actions<ChooseYearOfPlentyResource>(game_);
                require(!choices.empty(), "Year of Plenty has no resource choice");
                apply(choices.front());
                break;
            }
            default:
                return;
            }
        }
    }

    Resources currentObjective() const {
        if (nextDevelopmentCard_ < DevelopmentPlan.size()) return DevelopmentCost;
        const auto& state = game_.state();
        if (buildingCount(state, PlayerId::One) < 5) {
            if (!actions<BuildSettlement>(game_).empty()) return SettlementCost;
            return RoadCost;
        }
        if (state.awards.longestRoad != std::optional<PlayerId>{PlayerId::One}) return RoadCost;
        if (buildingCount(state, PlayerId::One, BuildingType::Settlement) > 0) return CityCost;
        return DevelopmentCost;
    }

    bool tradeToward(const Resources& target) {
        const auto player = game_.decisionPlayer();
        const auto& state = game_.state();
        const auto& hand = state.players[index(player)].resources;
        for (std::size_t receive = 0; receive < ResourceCount; ++receive) {
            if (hand[receive] >= target[receive]) continue;
            for (const auto trade : actions<MaritimeTrade>(game_)) {
                if (index(trade.receive) != receive) continue;
                const auto give = index(trade.give);
                const auto rate = maritimeRate(state, player, trade.give);
                if (hand[give] >= static_cast<unsigned>(target[give]) + rate) {
                    apply(trade);
                    return true;
                }
            }
        }
        return false;
    }

    bool canAfford(const Resources& target) const {
        const auto& hand = game_.state().players[index(game_.decisionPlayer())].resources;
        for (std::size_t r = 0; r < ResourceCount; ++r) if (hand[r] < target[r]) return false;
        return true;
    }

    void playAvailableDevelopmentCard() {
        if (const auto action = firstAction<PlayRoadBuilding>(game_)) apply(*action);
        else if (const auto action = firstAction<PlayYearOfPlenty>(game_)) apply(*action);
        else if (const auto choices = actions<PlayMonopoly>(game_); !choices.empty()) {
            const auto& victim = game_.state().players[index(PlayerId::Two)].resources;
            const auto best = *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
                return victim[index(left.resource)] < victim[index(right.resource)];
            });
            apply(best);
        } else if (const auto action = firstAction<PlayKnight>(game_)) apply(*action);
        resolveForcedPhases();
    }

    void buyPlannedDevelopmentCard() {
        apply(BuyDevelopmentCard{});
        require(game_.state().turn.phase == Phase::DevelopmentDraw, "buy did not enter DevelopmentDraw");
        const auto card = DevelopmentPlan[nextDevelopmentCard_++];
        const auto draw = DrawDevelopmentCard{card};
        const auto legal = game_.legalActions();
        require(std::find(legal.begin(), legal.end(), Action{draw}) != legal.end(),
                "planned development card is unavailable");
        apply(draw);
    }

    BuildSettlement bestBuildSettlement() const {
        const auto choices = actions<BuildSettlement>(game_);
        require(!choices.empty(), "no legal settlement build");
        return *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
            return vertexScore(game_.state(), left.vertex) < vertexScore(game_.state(), right.vertex);
        });
    }

    BuildCity bestBuildCity() const {
        const auto choices = actions<BuildCity>(game_);
        require(!choices.empty(), "no legal city build");
        return *std::max_element(choices.begin(), choices.end(), [&](const auto& left, const auto& right) {
            return vertexScore(game_.state(), left.vertex) < vertexScore(game_.state(), right.vertex);
        });
    }

    void playerOneMainPhase() {
        if (nextDevelopmentCard_ < DevelopmentPlan.size()) {
            while (tradeToward(DevelopmentCost)) {}
            if (canAfford(DevelopmentCost)) buyPlannedDevelopmentCard();
            return;
        }

        const unsigned buildings = buildingCount(game_.state(), PlayerId::One);
        if (buildings < 5) {
            if (!actions<BuildSettlement>(game_).empty()) {
                while (tradeToward(SettlementCost)) {}
                if (canAfford(SettlementCost)) apply(bestBuildSettlement());
            } else {
                while (tradeToward(RoadCost)) {}
                if (canAfford(RoadCost) && firstAction<BuildRoad>(game_)) apply(bestRoad<BuildRoad>());
            }
            return;
        }

        if (game_.state().awards.longestRoad != std::optional<PlayerId>{PlayerId::One}) {
            while (tradeToward(RoadCost)) {}
            if (canAfford(RoadCost) && firstAction<BuildRoad>(game_)) apply(bestRoad<BuildRoad>());
            return;
        }

        if (buildingCount(game_.state(), PlayerId::One, BuildingType::Settlement) > 0) {
            while (tradeToward(CityCost)) {}
            if (canAfford(CityCost) && firstAction<BuildCity>(game_)) apply(bestBuildCity());
        }
    }

    void playerTwoMainPhase() {
        if (buildingCount(game_.state(), PlayerId::Two) >= 3) return;
        if (!actions<BuildSettlement>(game_).empty()) {
            while (tradeToward(SettlementCost)) {}
            if (canAfford(SettlementCost)) apply(bestBuildSettlement());
        } else {
            while (tradeToward(RoadCost)) {}
            if (canAfford(RoadCost) && firstAction<BuildRoad>(game_)) apply(bestRoad<BuildRoad>());
        }
    }

    void playTurn() {
        require(game_.state().turn.phase == Phase::AwaitRoll, "turn did not start at AwaitRoll");
        const PlayerId player = game_.state().activePlayer;
        if (player == PlayerId::One) playAvailableDevelopmentCard();
        if (game_.state().turn.phase == Phase::Finished) return;

        const Resources target = player == PlayerId::One ? currentObjective() : SettlementCost;
        beginAndRoll(target);
        require(game_.state().turn.phase == Phase::Main, "roll did not resolve to Main");

        if (player == PlayerId::One) playerOneMainPhase();
        else playerTwoMainPhase();
        if (game_.state().turn.phase != Phase::Finished) apply(EndTurn{});
        ++turnsPlayed_;
    }
};

} // namespace

int main() {
    try {
        FullGameScenario scenario;
        scenario.run();
        scenario.report();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "full-game system test failed: " << error.what() << '\n';
        return 1;
    }
}
