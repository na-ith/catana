#include "catan/Rules.h"

#include <algorithm>
#include <array>
#include <functional>

namespace catan {
namespace {

std::uint8_t buildingCount(const GameState& state, PlayerId player, BuildingType type) {
    return static_cast<std::uint8_t>(std::count_if(state.board.buildings.begin(), state.board.buildings.end(),
        [&](const auto& building) { return building && building->owner == player && building->type == type; }));
}

std::uint8_t roadCount(const GameState& state, PlayerId player) {
    return static_cast<std::uint8_t>(std::count_if(state.board.roads.begin(), state.board.roads.end(),
        [&](const auto& road) { return road && road->owner == player; }));
}

PortType portFor(Resource resource) {
    return static_cast<PortType>(static_cast<std::uint8_t>(resource) + 1);
}

} // namespace

bool canAfford(const PlayerState& player, const Resources& cost) {
    for (std::size_t i = 0; i < ResourceCount; ++i)
        if (player.resources[i] < cost[i]) return false;
    return true;
}

std::uint8_t maritimeRate(const GameState& state, PlayerId player, Resource resource) {
    std::uint8_t rate = 4;
    const auto& topology = BoardTopology::standard();
    for (const auto& port : state.board.ports) {
        const auto& edge = topology.edges[port.edge.value];
        bool ownsPort = false;
        for (VertexId vertex : edge.vertices) {
            const auto& building = state.board.buildings[vertex.value];
            ownsPort |= building && building->owner == player;
        }
        if (!ownsPort) continue;
        if (port.type == portFor(resource)) return 2;
        if (port.type == PortType::Generic) rate = 3;
    }
    return rate;
}

bool canBuildSettlement(const GameState& state, PlayerId player, VertexId vertex, bool initial) {
    if (vertex.value >= BoardTopology::VertexCount || state.board.buildings[vertex.value]) return false;
    if (!initial && buildingCount(state, player, BuildingType::Settlement) >= 5) return false;
    const auto& topology = BoardTopology::standard();
    for (const auto neighbour : topology.vertices[vertex.value].neighbours)
        if (neighbour && state.board.buildings[neighbour->value]) return false;
    if (initial) return true;
    for (const auto edge : topology.vertices[vertex.value].edges) {
        if (edge && state.board.roads[edge->value] && state.board.roads[edge->value]->owner == player) return true;
    }
    return false;
}

bool canBuildRoad(const GameState& state, PlayerId player, EdgeId edge) {
    if (edge.value >= BoardTopology::EdgeCount || state.board.roads[edge.value]) return false;
    if (roadCount(state, player) >= 15) return false;
    const auto& topology = BoardTopology::standard();
    for (VertexId vertex : topology.edges[edge.value].vertices) {
        const auto& building = state.board.buildings[vertex.value];
        if (building && building->owner == player) return true;
        if (building && building->owner != player) continue;
        for (const auto adjacent : topology.vertices[vertex.value].edges) {
            if (adjacent && state.board.roads[adjacent->value] && state.board.roads[adjacent->value]->owner == player)
                return true;
        }
    }
    return false;
}

bool canBuildCity(const GameState& state, PlayerId player, VertexId vertex) {
    if (vertex.value >= BoardTopology::VertexCount || buildingCount(state, player, BuildingType::City) >= 4) return false;
    const auto& building = state.board.buildings[vertex.value];
    return building && building->owner == player && building->type == BuildingType::Settlement;
}

std::uint8_t longestRoad(const GameState& state, PlayerId player) {
    const auto& topology = BoardTopology::standard();
    std::array<bool, BoardTopology::EdgeCount> used{};
    std::function<unsigned(VertexId, unsigned)> visit = [&](VertexId vertex, unsigned length) -> unsigned {
        unsigned best = length;
        const auto& building = state.board.buildings[vertex.value];
        if (length > 0 && building && building->owner != player) return best;
        for (const auto candidate : topology.vertices[vertex.value].edges) {
            if (!candidate || used[candidate->value]) continue;
            const auto& road = state.board.roads[candidate->value];
            if (!road || road->owner != player) continue;
            used[candidate->value] = true;
            const auto& ends = topology.edges[candidate->value].vertices;
            const VertexId next = ends[0] == vertex ? ends[1] : ends[0];
            best = std::max(best, visit(next, length + 1));
            used[candidate->value] = false;
        }
        return best;
    };

    unsigned result = 0;
    for (std::size_t v = 0; v < BoardTopology::VertexCount; ++v)
        result = std::max(result, visit(VertexId{static_cast<std::uint8_t>(v)}, 0));
    return static_cast<std::uint8_t>(result);
}

bool robberMayTarget(const GameState& state, PlayerId player, HexId hex) {
    if (hex.value >= BoardTopology::HexCount || hex == state.board.robberHex) return false;
    const PlayerId other = opponent(player);
    if (publicVictoryPoints(state, other) > 2) return true;
    const auto& topology = BoardTopology::standard();
    for (VertexId vertex : topology.hexes[hex.value].vertices) {
        const auto& building = state.board.buildings[vertex.value];
        if (building && building->owner == other) return false;
    }
    return true;
}

bool validateState(const GameState& state, std::string* reason) {
    auto fail = [&](const char* text) {
        if (reason) *reason = text;
        return false;
    };
    if (state.board.robberHex.value >= BoardTopology::HexCount) return fail("robber hex is invalid");
    for (auto cards : state.bank.cards) if (cards > 19) return fail("bank contains more than 19 resource cards");
    for (std::size_t p = 0; p < PlayerCount; ++p) {
        const auto player = static_cast<PlayerId>(p);
        if (buildingCount(state, player, BuildingType::Settlement) > 5) return fail("too many settlements");
        if (buildingCount(state, player, BuildingType::City) > 4) return fail("too many cities");
        if (roadCount(state, player) > 15) return fail("too many roads");
    }
    for (std::size_t r = 0; r < ResourceCount; ++r) {
        const unsigned cards = state.bank.cards[r] + state.players[0].resources[r] + state.players[1].resources[r];
        if (cards != 19) return fail("resource conservation violated");
    }
    if (state.winner && state.turn.phase != Phase::Finished) return fail("winner set outside finished phase");
    return true;
}

} // namespace catan
