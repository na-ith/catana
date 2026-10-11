#include "TestHelpers.h"

#include <array>
#include <set>

using namespace catan;
using namespace test_support;

TEST(board_topology_has_standard_counts_and_bidirectional_links) {
    const auto& topology = BoardTopology::standard();
    CHECK_EQ(topology.hexes.size(), std::size_t{19});
    CHECK_EQ(topology.vertices.size(), std::size_t{54});
    CHECK_EQ(topology.edges.size(), std::size_t{72});

    unsigned boundaryEdges = 0;
    for (std::size_t e = 0; e < topology.edges.size(); ++e) {
        const auto& edge = topology.edges[e];
        CHECK(edge.vertices[0] != edge.vertices[1]);
        REQUIRE(edge.hexes[0].has_value());
        if (!edge.hexes[1]) ++boundaryEdges;
        for (auto endpoint : edge.vertices) {
            bool linkedBack = false;
            for (auto candidate : topology.vertices[endpoint.value].edges)
                linkedBack |= candidate && candidate->value == e;
            CHECK(linkedBack);
        }
    }
    CHECK_EQ(boundaryEdges, 30U);

    for (std::size_t h = 0; h < topology.hexes.size(); ++h) {
        std::set<std::uint8_t> vertices;
        std::set<std::uint8_t> edges;
        for (auto vertex : topology.hexes[h].vertices) vertices.insert(vertex.value);
        for (auto edge : topology.hexes[h].edges) edges.insert(edge.value);
        CHECK_EQ(vertices.size(), std::size_t{6});
        CHECK_EQ(edges.size(), std::size_t{6});
    }
}

TEST(generated_boards_have_standard_tile_token_and_port_inventories) {
    constexpr std::array<unsigned, 6> terrainCounts{4, 3, 4, 4, 3, 1};
    constexpr std::array<unsigned, 13> tokenCounts{0, 0, 1, 2, 2, 2, 2, 0, 2, 2, 2, 2, 1};
    constexpr std::array<unsigned, 6> portCounts{4, 1, 1, 1, 1, 1};
    const auto& topology = BoardTopology::standard();

    for (std::uint32_t seed = 0; seed < 64; ++seed) {
        const auto board = makeStandardBoard(seed);
        std::array<unsigned, 6> terrains{};
        std::array<unsigned, 13> tokens{};
        std::array<unsigned, 6> ports{};
        unsigned deserts = 0;
        for (std::size_t h = 0; h < board.hexes.size(); ++h) {
            const auto& tile = board.hexes[h];
            ++terrains[static_cast<std::size_t>(tile.terrain)];
            if (tile.number <= 12) ++tokens[tile.number];
            if (tile.terrain == Terrain::Desert) {
                ++deserts;
                CHECK_EQ(tile.number, std::uint8_t{0});
                CHECK_EQ(board.robberHex.value, static_cast<std::uint8_t>(h));
            } else {
                CHECK(tile.number >= 2 && tile.number <= 12 && tile.number != 7);
            }
        }
        for (const auto& port : board.ports) {
            ++ports[static_cast<std::size_t>(port.type)];
            CHECK(!topology.edges[port.edge.value].hexes[1].has_value());
        }
        CHECK_EQ(terrains, terrainCounts);
        CHECK_EQ(ports, portCounts);
        CHECK_EQ(deserts, 1U);
        CHECK_EQ(tokens[0], 1U); // desert
        for (std::size_t number = 2; number <= 12; ++number)
            if (number != 7) CHECK_EQ(tokens[number], tokenCounts[number]);

        for (const auto& edge : topology.edges) {
            if (!edge.hexes[1]) continue;
            const auto first = board.hexes[edge.hexes[0]->value].number;
            const auto second = board.hexes[edge.hexes[1]->value].number;
            CHECK(!((first == 6 || first == 8) && (second == 6 || second == 8)));
        }
    }
}

TEST(board_seed_is_reproducible) {
    const auto first = makeStandardBoard(123456);
    const auto again = makeStandardBoard(123456);
    const auto different = makeStandardBoard(123457);
    bool differs = false;
    for (std::size_t h = 0; h < first.hexes.size(); ++h) {
        CHECK_EQ(first.hexes[h].terrain, again.hexes[h].terrain);
        CHECK_EQ(first.hexes[h].number, again.hexes[h].number);
        differs |= first.hexes[h].terrain != different.hexes[h].terrain ||
                   first.hexes[h].number != different.hexes[h].number;
    }
    CHECK(differs);
}

TEST(generated_board_numbers_follow_rotated_standard_spirals_and_skip_the_desert) {
    // Reference rings in counterclockwise order, starting at the right-hand corner.
    constexpr std::array<std::uint8_t, 12> outer{11, 6, 2, 1, 0, 3, 7, 12, 16, 17, 18, 15};
    constexpr std::array<std::uint8_t, 6> inner{10, 5, 4, 8, 13, 14};
    constexpr std::array<std::uint8_t, 18> tokens{
        5, 2, 6, 3, 8, 10, 9, 12, 11, 4, 8, 10, 9, 4, 5, 6, 3, 11
    };
    std::array<std::array<bool, BoardTopology::HexCount>, 6> seen{};
    for (std::uint32_t seed = 0; seed < 4096; ++seed) {
        const auto board = makeStandardBoard(seed);
        CHECK(validateInitialBoard(board));
        unsigned matches = 0;
        for (std::size_t rotation = 0; rotation < seen.size(); ++rotation) {
            std::array<std::uint8_t, BoardTopology::HexCount> expected{};
            std::size_t token = 0;
            auto assign = [&](std::uint8_t hex) {
                if (hex != board.robberHex.value) expected[hex] = tokens[token++];
            };
            for (std::size_t i = 0; i < outer.size(); ++i)
                assign(outer[(i + 2 * rotation) % outer.size()]);
            for (std::size_t i = 0; i < inner.size(); ++i)
                assign(inner[(i + rotation) % inner.size()]);
            assign(9); // Center; it gets R (11) unless it is the desert.
            bool matching = true;
            for (std::size_t hex = 0; hex < expected.size(); ++hex)
                matching &= board.hexes[hex].number == expected[hex];
            if (matching) {
                ++matches;
                seen[rotation][board.robberHex.value] = true;
            }
        }
        CHECK_EQ(matches, 1U);

        for (const auto& edge : BoardTopology::standard().edges) {
            if (!edge.hexes[1]) continue;
            const auto a = board.hexes[edge.hexes[0]->value].number;
            const auto b = board.hexes[edge.hexes[1]->value].number;
            CHECK(!((a == 6 || a == 8) && (b == 6 || b == 8)));
        }
    }
    for (const auto& orientation : seen)
        for (const auto desertSeen : orientation) CHECK(desertSeen);
}

TEST(terrain_maps_to_the_correct_resource) {
    CHECK_EQ(producedResource(Terrain::Forest), std::optional<Resource>{Resource::Lumber});
    CHECK_EQ(producedResource(Terrain::Hills), std::optional<Resource>{Resource::Brick});
    CHECK_EQ(producedResource(Terrain::Pasture), std::optional<Resource>{Resource::Wool});
    CHECK_EQ(producedResource(Terrain::Fields), std::optional<Resource>{Resource::Grain});
    CHECK_EQ(producedResource(Terrain::Mountains), std::optional<Resource>{Resource::Ore});
    CHECK(!producedResource(Terrain::Desert));
}

TEST(initial_placement_uses_one_two_two_one_snake_order) {
    Game game(5);
    constexpr std::array<PlayerId, 4> order{PlayerId::One, PlayerId::Two, PlayerId::Two, PlayerId::One};
    for (std::size_t placement = 0; placement < order.size(); ++placement) {
        CHECK_EQ(game.state().turn.phase, Phase::InitialSettlement);
        CHECK_EQ(game.decisionPlayer(), order[placement]);
        accepted(game, firstAction<PlaceSettlement>(game));
        CHECK_EQ(game.state().turn.phase, Phase::InitialRoad);
        CHECK_EQ(game.decisionPlayer(), order[placement]);
        accepted(game, firstAction<PlaceRoad>(game));
    }
    CHECK_EQ(game.state().turn.phase, Phase::AwaitRoll);
    CHECK_EQ(game.state().activePlayer, PlayerId::One);
    CHECK_EQ(game.state().turnNumber, std::uint32_t{1});
    CHECK_EQ(buildingCount(game.state(), PlayerId::One), 2U);
    CHECK_EQ(buildingCount(game.state(), PlayerId::Two), 2U);
}

TEST(initial_road_must_touch_the_settlement_just_placed) {
    Game game(6);
    const auto settlement = firstAction<PlaceSettlement>(game);
    accepted(game, settlement);
    const auto roads = actions<PlaceRoad>(game);
    REQUIRE(!roads.empty());
    const auto& topology = BoardTopology::standard();
    for (const auto road : roads) {
        const auto ends = topology.edges[road.edge.value].vertices;
        CHECK(ends[0] == settlement.vertex || ends[1] == settlement.vertex);
    }
    EdgeId unrelated{};
    bool found = false;
    for (std::size_t e = 0; e < BoardTopology::EdgeCount; ++e) {
        const auto ends = topology.edges[e].vertices;
        if (ends[0] != settlement.vertex && ends[1] != settlement.vertex) {
            unrelated = EdgeId{static_cast<std::uint8_t>(e)};
            found = true;
            break;
        }
    }
    REQUIRE(found);
    CHECK(!game.transform(PlaceRoad{unrelated}).accepted);
}

TEST(settlement_distance_rule_applies_during_setup) {
    Game game(7);
    const auto first = firstAction<PlaceSettlement>(game);
    accepted(game, first);
    accepted(game, firstAction<PlaceRoad>(game));
    const auto& topology = BoardTopology::standard();
    for (auto neighbour : topology.vertices[first.vertex.value].neighbours) {
        if (!neighbour) continue;
        bool offered = false;
        for (const auto action : actions<PlaceSettlement>(game)) offered |= action.vertex == *neighbour;
        CHECK(!offered);
    }
}

TEST(only_second_settlements_grant_adjacent_non_desert_resources) {
    Game game(73);
    constexpr std::array<PlayerId, 4> order{PlayerId::One, PlayerId::Two, PlayerId::Two, PlayerId::One};
    const auto& topology = BoardTopology::standard();
    for (std::size_t placement = 0; placement < order.size(); ++placement) {
        const auto player = order[placement];
        const auto settlement = firstAction<PlaceSettlement>(game);
        const auto before = game.state().players[index(player)].resources;
        Resources expected{};
        if (placement >= 2) {
            for (auto hex : topology.vertices[settlement.vertex.value].hexes) {
                if (!hex) continue;
                const auto resource = producedResource(game.state().board.hexes[hex->value].terrain);
                if (resource) ++expected[index(*resource)];
            }
        }
        accepted(game, settlement);
        accepted(game, firstAction<PlaceRoad>(game));
        const auto after = game.state().players[index(player)].resources;
        for (std::size_t resource = 0; resource < ResourceCount; ++resource)
            CHECK_EQ(static_cast<unsigned>(after[resource] - before[resource]), static_cast<unsigned>(expected[resource]));
    }
    std::string reason;
    CHECK(validateState(game.state(), &reason));
}
