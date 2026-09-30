#include "TestHelpers.h"

using namespace catan;
using namespace test_support;

namespace {

GameState singleRollBoard(Terrain terrain, std::uint8_t number = 5) {
    auto state = blankState(Phase::AwaitRoll, 20);
    for (auto& hex : state.board.hexes) hex.number = 0;
    state.board.hexes[0] = HexState{terrain, number};
    state.board.robberHex = HexId{1};
    return state;
}

} // namespace

TEST(settlements_produce_one_and_cities_produce_two) {
    auto state = singleRollBoard(Terrain::Forest);
    const auto vertices = BoardTopology::standard().hexes[0].vertices;
    placeBuilding(state, vertices[0], PlayerId::One, BuildingType::Settlement);
    placeBuilding(state, vertices[2], PlayerId::Two, BuildingType::City);
    Game game(state);
    roll(game, 5);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Lumber)], std::uint8_t{1});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Lumber)], std::uint8_t{2});
    CHECK_EQ(game.state().bank.cards[index(Resource::Lumber)], std::uint8_t{16});
}

TEST(robber_blocks_all_production_from_its_hex) {
    auto state = singleRollBoard(Terrain::Fields);
    state.board.robberHex = HexId{0};
    const auto vertices = BoardTopology::standard().hexes[0].vertices;
    placeBuilding(state, vertices[0], PlayerId::One, BuildingType::City);
    placeBuilding(state, vertices[2], PlayerId::Two, BuildingType::Settlement);
    Game game(state);
    roll(game, 5);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Grain)], std::uint8_t{0});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Grain)], std::uint8_t{0});
    CHECK_EQ(game.state().bank.cards[index(Resource::Grain)], std::uint8_t{19});
}

TEST(only_hexes_matching_the_roll_produce) {
    auto state = singleRollBoard(Terrain::Mountains, 9);
    const auto vertex = BoardTopology::standard().hexes[0].vertices[0];
    placeBuilding(state, vertex, PlayerId::One);
    Game game(state);
    roll(game, 8);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Ore)], std::uint8_t{0});
}

TEST(bank_shortage_withholds_that_resource_from_everyone) {
    auto state = singleRollBoard(Terrain::Forest);
    const auto vertices = BoardTopology::standard().hexes[0].vertices;
    placeBuilding(state, vertices[0], PlayerId::One);
    placeBuilding(state, vertices[2], PlayerId::Two);
    setResources(state, PlayerId::One, resources(18, 0, 0, 0, 0));
    Game game(state);
    roll(game, 5);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Lumber)], std::uint8_t{18});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Lumber)], std::uint8_t{0});
    CHECK_EQ(game.state().bank.cards[index(Resource::Lumber)], std::uint8_t{1});
}

TEST(single_player_bank_shortage_distributes_the_remaining_cards) {
    auto state = singleRollBoard(Terrain::Forest);
    const auto vertex = BoardTopology::standard().hexes[0].vertices[0];
    placeBuilding(state, vertex, PlayerId::One, BuildingType::City);
    setResources(state, PlayerId::Two, resources(18, 0, 0, 0, 0));
    Game game(state);
    roll(game, 5);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Lumber)], std::uint8_t{1});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Lumber)], std::uint8_t{18});
    CHECK_EQ(game.state().bank.cards[index(Resource::Lumber)], std::uint8_t{0});
}

TEST(shortage_of_one_resource_does_not_cancel_other_resources) {
    auto state = blankState(Phase::AwaitRoll, 21);
    for (auto& hex : state.board.hexes) hex.number = 0;
    state.board.hexes[0] = HexState{Terrain::Forest, 5};
    state.board.hexes[18] = HexState{Terrain::Pasture, 5};
    state.board.robberHex = HexId{1};
    const auto& topology = BoardTopology::standard();
    placeBuilding(state, topology.hexes[0].vertices[0], PlayerId::One, BuildingType::City);
    placeBuilding(state, topology.hexes[0].vertices[2], PlayerId::Two);
    placeBuilding(state, topology.hexes[18].vertices[0], PlayerId::Two);
    setResources(state, PlayerId::One, resources(18, 0, 0, 0, 0));
    Game game(state);
    roll(game, 5);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Lumber)], std::uint8_t{18});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Wool)], std::uint8_t{1});
}

TEST(building_costs_match_the_published_base_rules) {
    CHECK_EQ(RoadCost, resources(1, 1, 0, 0, 0));
    CHECK_EQ(SettlementCost, resources(1, 1, 1, 1, 0));
    CHECK_EQ(CityCost, resources(0, 0, 0, 2, 3));
    CHECK_EQ(DevelopmentCost, resources(0, 0, 1, 1, 1));
}

TEST(road_requires_connection_and_cannot_cross_an_opponent_building) {
    auto state = blankState();
    const auto path = simplePath(2);
    CHECK(!canBuildRoad(state, PlayerId::One, path.edges[0]));
    placeBuilding(state, path.vertices[0], PlayerId::One);
    CHECK(canBuildRoad(state, PlayerId::One, path.edges[0]));
    placeRoad(state, path.edges[0], PlayerId::One);
    placeBuilding(state, path.vertices[1], PlayerId::Two);
    CHECK(!canBuildRoad(state, PlayerId::One, path.edges[1]));
    state.board.buildings[path.vertices[1].value] = Building{PlayerId::One, BuildingType::Settlement};
    CHECK(canBuildRoad(state, PlayerId::One, path.edges[1]));
}

TEST(normal_settlement_requires_own_road_and_respects_distance_rule) {
    auto state = blankState();
    const auto path = simplePath(2);
    placeRoad(state, path.edges[0], PlayerId::One);
    placeRoad(state, path.edges[1], PlayerId::One);
    placeBuilding(state, path.vertices[0], PlayerId::One);
    CHECK(!canBuildSettlement(state, PlayerId::One, path.vertices[1], false));
    CHECK(canBuildSettlement(state, PlayerId::One, path.vertices[2], false));
    state.board.roads[path.edges[1].value]->owner = PlayerId::Two;
    CHECK(!canBuildSettlement(state, PlayerId::One, path.vertices[2], false));
}

TEST(city_can_only_upgrade_the_players_own_settlement) {
    auto state = blankState();
    placeBuilding(state, VertexId{0}, PlayerId::One);
    placeBuilding(state, VertexId{10}, PlayerId::Two);
    CHECK(canBuildCity(state, PlayerId::One, VertexId{0}));
    CHECK(!canBuildCity(state, PlayerId::One, VertexId{10}));
    state.board.buildings[0]->type = BuildingType::City;
    CHECK(!canBuildCity(state, PlayerId::One, VertexId{0}));
}

TEST(paid_build_returns_exact_cost_to_the_bank) {
    auto state = blankState();
    const auto path = simplePath(1);
    placeBuilding(state, path.vertices[0], PlayerId::One);
    setResources(state, PlayerId::One, RoadCost);
    Game game(state);
    const auto bankBefore = game.state().bank.cards;
    accepted(game, BuildRoad{path.edges[0]});
    CHECK_EQ(game.state().players[0].resources, Resources{});
    for (std::size_t r = 0; r < ResourceCount; ++r)
        CHECK_EQ(game.state().bank.cards[r], static_cast<std::uint8_t>(bankBefore[r] + RoadCost[r]));
}

TEST(piece_limits_are_fifteen_roads_five_settlements_and_four_cities) {
    auto state = blankState();
    for (std::size_t e = 0; e < 15; ++e) placeRoad(state, EdgeId{static_cast<std::uint8_t>(e)}, PlayerId::One);
    for (std::size_t e = 15; e < BoardTopology::EdgeCount; ++e)
        CHECK(!canBuildRoad(state, PlayerId::One, EdgeId{static_cast<std::uint8_t>(e)}));

    state.board.roads.fill(std::nullopt);
    for (std::size_t v : {0U, 10U, 20U, 30U, 40U}) placeBuilding(state, VertexId{static_cast<std::uint8_t>(v)}, PlayerId::One);
    CHECK(!canBuildSettlement(state, PlayerId::One, VertexId{50}, false));

    state.board.buildings.fill(std::nullopt);
    for (std::size_t v : {0U, 10U, 20U, 30U}) placeBuilding(state, VertexId{static_cast<std::uint8_t>(v)}, PlayerId::One, BuildingType::City);
    placeBuilding(state, VertexId{40}, PlayerId::One);
    CHECK(!canBuildCity(state, PlayerId::One, VertexId{40}));
}

TEST(maritime_rates_are_four_to_one_three_to_one_and_matching_two_to_one) {
    auto state = blankState();
    CHECK_EQ(maritimeRate(state, PlayerId::One, Resource::Lumber), std::uint8_t{4});
    const auto portEdge = state.board.ports[0].edge;
    const auto endpoint = BoardTopology::standard().edges[portEdge.value].vertices[0];
    placeBuilding(state, endpoint, PlayerId::One);
    state.board.ports[0].type = PortType::Generic;
    CHECK_EQ(maritimeRate(state, PlayerId::One, Resource::Lumber), std::uint8_t{3});
    state.board.ports[0].type = PortType::Lumber;
    CHECK_EQ(maritimeRate(state, PlayerId::One, Resource::Lumber), std::uint8_t{2});
    CHECK_EQ(maritimeRate(state, PlayerId::One, Resource::Brick), std::uint8_t{4});
    CHECK_EQ(maritimeRate(state, PlayerId::Two, Resource::Lumber), std::uint8_t{4});
}

TEST(maritime_trade_moves_cards_between_player_and_bank) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(4, 0, 0, 0, 0));
    Game game(state);
    accepted(game, MaritimeTrade{Resource::Lumber, Resource::Ore});
    CHECK_EQ(game.state().players[0].resources, resources(0, 0, 0, 0, 1));
    CHECK_EQ(game.state().bank.cards[index(Resource::Lumber)], std::uint8_t{19});
    CHECK_EQ(game.state().bank.cards[index(Resource::Ore)], std::uint8_t{18});
}

TEST(maritime_trade_requires_bank_stock_and_different_resource_types) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(4, 0, 0, 0, 0));
    setResources(state, PlayerId::Two, resources(0, 0, 0, 0, 19));
    Game game(state);
    bool oreTrade = false;
    bool sameTrade = false;
    for (const auto& action : actions<MaritimeTrade>(game)) {
        oreTrade |= action.give == Resource::Lumber && action.receive == Resource::Ore;
        sameTrade |= action.give == action.receive;
    }
    CHECK(!oreTrade);
    CHECK(!sameTrade);
}
