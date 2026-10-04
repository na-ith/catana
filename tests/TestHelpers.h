#pragma once

#include "TestHarness.h"
#include "catan/Game.h"

#include <algorithm>
#include <array>
#include <functional>
#include <numeric>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace test_support {

inline catan::GameState blankState(catan::Phase phase = catan::Phase::Main, std::uint32_t seed = 1) {
    catan::Game game(seed);
    auto state = game.state();
    state.board.buildings.fill(std::nullopt);
    state.board.roads.fill(std::nullopt);
    state.players = {};
    state.bank.cards.fill(19);
    state.developmentDeck = {};
    state.activePlayer = catan::PlayerId::One;
    state.turn = {};
    state.turn.phase = phase;
    state.turn.returnPhase = catan::Phase::Main;
    state.turn.decisionPlayer = catan::PlayerId::One;
    state.awards = {};
    state.turnNumber = 1;
    state.winner.reset();
    return state;
}

inline void setResources(catan::GameState& state, catan::PlayerId player, const catan::Resources& resources) {
    auto& hand = state.players[catan::index(player)].resources;
    for (std::size_t r = 0; r < catan::ResourceCount; ++r) {
        state.bank.cards[r] += hand[r];
        hand[r] = 0;
    }
    for (std::size_t r = 0; r < catan::ResourceCount; ++r) {
        if (resources[r] > state.bank.cards[r]) throw std::runtime_error("test requested unavailable resources");
        state.bank.cards[r] -= resources[r];
        hand[r] = resources[r];
    }
}

inline catan::Resources resources(std::uint8_t lumber, std::uint8_t brick, std::uint8_t wool,
                                  std::uint8_t grain, std::uint8_t ore) {
    return {lumber, brick, wool, grain, ore};
}

inline void placeBuilding(catan::GameState& state, catan::VertexId vertex, catan::PlayerId owner,
                          catan::BuildingType type = catan::BuildingType::Settlement) {
    state.board.buildings[vertex.value] = catan::Building{owner, type};
}

inline void placeRoad(catan::GameState& state, catan::EdgeId edge, catan::PlayerId owner) {
    state.board.roads[edge.value] = catan::Road{owner};
}

template <typename ActionType>
bool hasAction(const catan::Game& game) {
    const auto legal = game.legalTransformations();
    return std::any_of(legal.begin(), legal.end(),
                       [](const auto& action) { return std::holds_alternative<ActionType>(action); });
}

template <typename ActionType>
std::vector<ActionType> actions(const catan::Game& game) {
    std::vector<ActionType> result;
    for (const auto& action : game.legalTransformations())
        if (const auto* value = std::get_if<ActionType>(&action)) result.push_back(*value);
    return result;
}

template <typename ActionType>
ActionType firstAction(const catan::Game& game) {
    for (const auto& action : game.legalTransformations())
        if (const auto* value = std::get_if<ActionType>(&action)) return *value;
    throw std::runtime_error("expected action was not legal");
}

inline void accepted(catan::Game& game, const catan::Action& action) {
    const auto result = game.transform(action);
    if (!result.accepted) throw std::runtime_error("expected accepted action: " + result.message);
}

inline void roll(catan::Game& game, std::uint8_t sum) {
    if (game.state().turn.phase == catan::Phase::AwaitRoll) accepted(game, catan::BeginRoll{});
    accepted(game, catan::RollDice{sum});
}

inline void finishPlacements(catan::Game& game) {
    for (int placement = 0; placement < 4; ++placement) {
        accepted(game, firstAction<catan::PlaceSettlement>(game));
        accepted(game, firstAction<catan::PlaceRoad>(game));
    }
}

struct Path {
    std::vector<catan::VertexId> vertices;
    std::vector<catan::EdgeId> edges;
};

inline Path simplePath(std::size_t edgeCount, std::optional<catan::VertexId> requestedStart = std::nullopt) {
    const auto& topology = catan::BoardTopology::standard();
    Path path;
    std::array<bool, catan::BoardTopology::VertexCount> visited{};
    std::function<bool(catan::VertexId)> search = [&](catan::VertexId vertex) {
        if (path.edges.size() == edgeCount) return true;
        for (auto edge : topology.vertices[vertex.value].edges) {
            if (!edge) continue;
            const auto ends = topology.edges[edge->value].vertices;
            const auto next = ends[0] == vertex ? ends[1] : ends[0];
            if (visited[next.value]) continue;
            visited[next.value] = true;
            path.edges.push_back(*edge);
            path.vertices.push_back(next);
            if (search(next)) return true;
            path.vertices.pop_back();
            path.edges.pop_back();
            visited[next.value] = false;
        }
        return false;
    };
    for (std::size_t start = 0; start < catan::BoardTopology::VertexCount; ++start) {
        if (requestedStart && requestedStart->value != start) continue;
        path = {};
        path.vertices.push_back(catan::VertexId{static_cast<std::uint8_t>(start)});
        visited.fill(false);
        visited[start] = true;
        if (search(path.vertices.front())) return path;
    }
    throw std::runtime_error("could not find simple path in board topology");
}

inline std::optional<catan::EdgeId> edgeBetween(catan::VertexId left, catan::VertexId right) {
    const auto& topology = catan::BoardTopology::standard();
    for (auto edge : topology.vertices[left.value].edges) {
        if (!edge) continue;
        const auto ends = topology.edges[edge->value].vertices;
        if (ends[0] == right || ends[1] == right) return edge;
    }
    return std::nullopt;
}

inline double probabilityOf(const catan::Game& game, const catan::Action& action) {
    for (const auto& outcome : game.chanceOutcomes()) if (outcome.action == action) return outcome.probability;
    return 0.0;
}

inline unsigned buildingCount(const catan::GameState& state, catan::PlayerId player,
                              std::optional<catan::BuildingType> type = std::nullopt) {
    return static_cast<unsigned>(std::count_if(state.board.buildings.begin(), state.board.buildings.end(),
        [&](const auto& building) {
            return building && building->owner == player && (!type || building->type == *type);
        }));
}

} // namespace test_support
