#include "TestHelpers.h"

#include <set>

using namespace catan;
using namespace test_support;

namespace {

Path pathAvoiding(std::size_t length, const std::set<std::uint8_t>& excludedEdges) {
    const auto& topology = BoardTopology::standard();
    Path path;
    std::array<bool, BoardTopology::VertexCount> visited{};
    std::function<bool(VertexId)> search = [&](VertexId vertex) {
        if (path.edges.size() == length) return true;
        for (auto edge : topology.vertices[vertex.value].edges) {
            if (!edge || excludedEdges.contains(edge->value)) continue;
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
    for (std::size_t start = 0; start < BoardTopology::VertexCount; ++start) {
        path = {};
        path.vertices.push_back(VertexId{static_cast<std::uint8_t>(start)});
        visited.fill(false);
        visited[start] = true;
        if (search(path.vertices.front())) return path;
    }
    throw std::runtime_error("no edge-disjoint path found");
}

void giveOldCard(GameState& state, PlayerId player, DevelopmentCard card, std::uint8_t amount = 1) {
    state.players[index(player)].developmentCards.cards[index(card)] = amount;
    state.players[index(player)].developmentCards.boughtThisTurn[index(card)] = 0;
    state.developmentDeck.cards[index(card)] = static_cast<std::uint8_t>(
        state.developmentDeck.cards[index(card)] >= amount
            ? state.developmentDeck.cards[index(card)] - amount : 0);
}

HexId differentHex(HexId current) {
    return HexId{static_cast<std::uint8_t>(current.value == 0 ? 1 : 0)};
}

} // namespace

TEST(development_deck_has_the_published_twenty_five_card_composition) {
    DevelopmentDeckState deck;
    CHECK_EQ(deck.cards[index(DevelopmentCard::Knight)], std::uint8_t{14});
    CHECK_EQ(deck.cards[index(DevelopmentCard::RoadBuilding)], std::uint8_t{2});
    CHECK_EQ(deck.cards[index(DevelopmentCard::YearOfPlenty)], std::uint8_t{2});
    CHECK_EQ(deck.cards[index(DevelopmentCard::Monopoly)], std::uint8_t{2});
    CHECK_EQ(deck.cards[index(DevelopmentCard::VictoryPoint)], std::uint8_t{5});
    CHECK_EQ(std::accumulate(deck.cards.begin(), deck.cards.end(), 0U), 25U);
}

TEST(buying_a_development_card_pays_cost_and_enters_a_chance_node) {
    auto state = blankState();
    setResources(state, PlayerId::One, DevelopmentCost);
    Game game(state);
    accepted(game, BuyDevelopmentCard{});
    CHECK_EQ(game.state().turn.phase, Phase::DevelopmentDraw);
    CHECK_EQ(game.state().players[0].resources, Resources{});
    CHECK_EQ(game.chanceOutcomes().size(), std::size_t{5});
    CHECK_NEAR(probabilityOf(game, DrawDevelopmentCard{DevelopmentCard::Knight}), 14.0 / 25.0, 1e-12);
    CHECK_NEAR(probabilityOf(game, DrawDevelopmentCard{DevelopmentCard::VictoryPoint}), 5.0 / 25.0, 1e-12);
}

TEST(newly_bought_development_card_cannot_be_played_until_a_later_turn) {
    auto state = blankState();
    setResources(state, PlayerId::One, DevelopmentCost);
    Game game(state);
    accepted(game, BuyDevelopmentCard{});
    accepted(game, DrawDevelopmentCard{DevelopmentCard::Knight});
    CHECK(!hasAction<PlayKnight>(game));
    accepted(game, EndTurn{});
    roll(game, 6);
    accepted(game, EndTurn{});
    CHECK_EQ(game.state().activePlayer, PlayerId::One);
    CHECK(hasAction<PlayKnight>(game));
}

TEST(only_one_non_victory_development_card_can_be_played_per_turn) {
    auto state = blankState();
    giveOldCard(state, PlayerId::One, DevelopmentCard::Monopoly);
    giveOldCard(state, PlayerId::One, DevelopmentCard::Knight);
    Game game(state);
    accepted(game, PlayMonopoly{Resource::Ore});
    CHECK(game.state().turn.playedDevelopmentCard);
    CHECK(!hasAction<PlayKnight>(game));
    CHECK(!hasAction<PlayMonopoly>(game));
}

TEST(development_cards_are_playable_before_or_after_the_roll) {
    auto before = blankState(Phase::AwaitRoll);
    giveOldCard(before, PlayerId::One, DevelopmentCard::Monopoly);
    Game preRoll(before);
    CHECK(hasAction<PlayMonopoly>(preRoll));

    auto after = blankState(Phase::Main);
    giveOldCard(after, PlayerId::One, DevelopmentCard::Monopoly);
    Game postRoll(after);
    CHECK(hasAction<PlayMonopoly>(postRoll));
}

TEST(knight_moves_robber_increments_count_and_returns_to_previous_phase) {
    auto state = blankState(Phase::AwaitRoll);
    giveOldCard(state, PlayerId::One, DevelopmentCard::Knight);
    Game game(state);
    accepted(game, PlayKnight{});
    CHECK_EQ(game.state().players[0].playedKnights, std::uint8_t{1});
    CHECK_EQ(game.state().turn.phase, Phase::MoveRobber);
    accepted(game, MoveRobber{differentHex(game.state().board.robberHex)});
    CHECK_EQ(game.state().turn.phase, Phase::AwaitRoll);
    CHECK(game.state().turn.playedDevelopmentCard);
}

TEST(road_building_places_two_free_roads_without_spending_resources) {
    auto state = blankState();
    const auto path = simplePath(2);
    placeBuilding(state, path.vertices[0], PlayerId::One);
    giveOldCard(state, PlayerId::One, DevelopmentCard::RoadBuilding);
    const auto bankBefore = state.bank.cards;
    Game game(state);
    accepted(game, PlayRoadBuilding{});
    CHECK_EQ(game.state().turn.phase, Phase::FreeRoads);
    accepted(game, PlaceRoad{path.edges[0]});
    CHECK_EQ(game.state().turn.phase, Phase::FreeRoads);
    accepted(game, PlaceRoad{path.edges[1]});
    CHECK_EQ(game.state().turn.phase, Phase::Main);
    CHECK_EQ(game.state().bank.cards, bankBefore);
    CHECK_EQ(game.state().players[0].resources, Resources{});
}

TEST(year_of_plenty_takes_exactly_two_available_bank_resources) {
    auto state = blankState();
    giveOldCard(state, PlayerId::One, DevelopmentCard::YearOfPlenty);
    Game game(state);
    accepted(game, PlayYearOfPlenty{});
    CHECK_EQ(game.state().turn.phase, Phase::YearOfPlenty);
    accepted(game, ChooseYearOfPlentyResource{Resource::Ore});
    CHECK_EQ(game.state().turn.phase, Phase::YearOfPlenty);
    accepted(game, ChooseYearOfPlentyResource{Resource::Grain});
    CHECK_EQ(game.state().turn.phase, Phase::Main);
    CHECK_EQ(game.state().players[0].resources, resources(0, 0, 0, 1, 1));
}

TEST(year_of_plenty_cannot_choose_a_resource_missing_from_the_bank) {
    auto state = blankState();
    giveOldCard(state, PlayerId::One, DevelopmentCard::YearOfPlenty);
    setResources(state, PlayerId::Two, resources(0, 0, 0, 0, 19));
    Game game(state);
    accepted(game, PlayYearOfPlenty{});
    bool offersOre = false;
    for (const auto choice : actions<ChooseYearOfPlentyResource>(game)) offersOre |= choice.resource == Resource::Ore;
    CHECK(!offersOre);
}

TEST(monopoly_transfers_every_opponent_card_of_the_named_resource) {
    auto state = blankState();
    giveOldCard(state, PlayerId::One, DevelopmentCard::Monopoly);
    setResources(state, PlayerId::Two, resources(0, 0, 0, 0, 7));
    Game game(state);
    accepted(game, PlayMonopoly{Resource::Ore});
    CHECK_EQ(game.state().players[0].resources[index(Resource::Ore)], std::uint8_t{7});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Ore)], std::uint8_t{0});
}

TEST(victory_point_cards_score_secretly_and_are_not_playable_actions) {
    auto state = blankState();
    placeBuilding(state, VertexId{0}, PlayerId::One);
    state.players[0].developmentCards.cards[index(DevelopmentCard::VictoryPoint)] = 2;
    CHECK_EQ(publicVictoryPoints(state, PlayerId::One), std::uint8_t{1});
    CHECK_EQ(victoryPoints(state, PlayerId::One), std::uint8_t{3});
    Game game(state);
    for (const auto& action : game.legalTransformations())
        CHECK(!std::holds_alternative<DrawDevelopmentCard>(action));
}

TEST(largest_army_starts_at_three_retains_ties_and_transfers_only_when_surpassed) {
    auto state = blankState(Phase::AwaitRoll);
    state.players[0].playedKnights = 2;
    giveOldCard(state, PlayerId::One, DevelopmentCard::Knight);
    Game first(state);
    accepted(first, PlayKnight{});
    CHECK_EQ(first.state().awards.largestArmy, std::optional<PlayerId>{PlayerId::One});

    state = blankState(Phase::AwaitRoll);
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    state.awards.largestArmy = PlayerId::One;
    state.players[0].playedKnights = 3;
    state.players[1].playedKnights = 2;
    giveOldCard(state, PlayerId::Two, DevelopmentCard::Knight);
    Game tie(state);
    accepted(tie, PlayKnight{});
    CHECK_EQ(tie.state().awards.largestArmy, std::optional<PlayerId>{PlayerId::One});

    state.players[1].playedKnights = 3;
    state.players[1].developmentCards.cards[index(DevelopmentCard::Knight)] = 1;
    Game surpass(state);
    accepted(surpass, PlayKnight{});
    CHECK_EQ(surpass.state().awards.largestArmy, std::optional<PlayerId>{PlayerId::Two});
}

TEST(longest_road_counts_trails_cycles_and_opponent_interruptions) {
    auto state = blankState();
    const auto path = simplePath(6);
    for (auto edge : path.edges) placeRoad(state, edge, PlayerId::One);
    CHECK_EQ(longestRoad(state, PlayerId::One), std::uint8_t{6});
    placeBuilding(state, path.vertices[3], PlayerId::Two);
    CHECK_EQ(longestRoad(state, PlayerId::One), std::uint8_t{3});

    state.board.roads.fill(std::nullopt);
    state.board.buildings.fill(std::nullopt);
    const auto& hex = BoardTopology::standard().hexes[9];
    for (auto edge : hex.edges) placeRoad(state, edge, PlayerId::One);
    CHECK_EQ(longestRoad(state, PlayerId::One), std::uint8_t{6});
}

TEST(longest_road_counts_a_loop_starting_and_ending_at_an_opponent_settlement) {
    auto state = blankState();
    const auto& hex = BoardTopology::standard().hexes[9];
    for (auto edge : hex.edges) placeRoad(state, edge, PlayerId::One);
    placeBuilding(state, hex.vertices[0], PlayerId::Two);

    CHECK_EQ(longestRoad(state, PlayerId::One), std::uint8_t{6});
}

TEST(longest_road_branch_counts_the_best_trail_not_every_road) {
    auto state = blankState();
    const auto& topology = BoardTopology::standard();
    bool built = false;
    for (std::size_t centerIndex = 0; centerIndex < BoardTopology::VertexCount && !built; ++centerIndex) {
        const VertexId center{static_cast<std::uint8_t>(centerIndex)};
        std::vector<EdgeId> incident;
        for (auto edge : topology.vertices[center.value].edges) if (edge) incident.push_back(*edge);
        if (incident.size() != 3) continue;
        std::vector<EdgeId> extensions;
        std::set<std::uint8_t> outerVertices;
        bool valid = true;
        for (auto arm : incident) {
            const auto ends = topology.edges[arm.value].vertices;
            const auto neighbour = ends[0] == center ? ends[1] : ends[0];
            bool found = false;
            for (auto candidate : topology.vertices[neighbour.value].edges) {
                if (!candidate || *candidate == arm) continue;
                if (std::find(extensions.begin(), extensions.end(), *candidate) != extensions.end()) continue;
                const auto candidateEnds = topology.edges[candidate->value].vertices;
                const auto outer = candidateEnds[0] == neighbour ? candidateEnds[1] : candidateEnds[0];
                if (outer == center || outerVertices.contains(outer.value)) continue;
                extensions.push_back(*candidate);
                outerVertices.insert(outer.value);
                found = true;
                break;
            }
            if (!found) { valid = false; break; }
        }
        if (!valid) continue;
        for (auto edge : incident) placeRoad(state, edge, PlayerId::One);
        for (auto edge : extensions) placeRoad(state, edge, PlayerId::One);
        built = true;
    }
    REQUIRE(built);
    CHECK_EQ(longestRoad(state, PlayerId::One), std::uint8_t{4});
}

TEST(longest_road_starts_at_five_retains_tie_and_transfers_when_surpassed) {
    const auto firstPath = simplePath(5);
    std::set<std::uint8_t> excluded;
    for (auto edge : firstPath.edges) excluded.insert(edge.value);
    const auto secondPath = pathAvoiding(6, excluded);

    auto state = blankState();
    for (auto edge : firstPath.edges) placeRoad(state, edge, PlayerId::One);
    for (std::size_t i = 0; i < 4; ++i) placeRoad(state, secondPath.edges[i], PlayerId::Two);
    state.awards.longestRoad = PlayerId::One;
    state.awards.roadLengths = {5, 4};
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    setResources(state, PlayerId::Two, RoadCost);

    auto noHolderState = state;
    noHolderState.awards.longestRoad.reset();
    Game noHolderTie(noHolderState);
    accepted(noHolderTie, BuildRoad{secondPath.edges[4]});
    CHECK(!noHolderTie.state().awards.longestRoad);

    Game tie(state);
    accepted(tie, BuildRoad{secondPath.edges[4]});
    CHECK_EQ(tie.state().awards.roadLengths, (std::array<std::uint8_t, 2>{5, 5}));
    CHECK_EQ(tie.state().awards.longestRoad, std::optional<PlayerId>{PlayerId::One});

    state = tie.state();
    state.turn.phase = Phase::Main;
    state.turn.decisionPlayer = PlayerId::Two;
    state.activePlayer = PlayerId::Two;
    setResources(state, PlayerId::Two, RoadCost);
    Game surpass(state);
    accepted(surpass, BuildRoad{secondPath.edges[5]});
    CHECK_EQ(surpass.state().awards.longestRoad, std::optional<PlayerId>{PlayerId::Two});
}

TEST(longest_road_is_removed_when_an_opponent_settlement_breaks_it_below_five) {
    Path path;
    const auto& topology = BoardTopology::standard();
    for (std::size_t start = 0; start < BoardTopology::VertexCount; ++start) {
        auto candidate = simplePath(6, VertexId{static_cast<std::uint8_t>(start)});
        unsigned degree = 0;
        for (auto edge : topology.vertices[candidate.vertices[3].value].edges) degree += edge.has_value();
        if (degree == 3) { path = std::move(candidate); break; }
    }
    REQUIRE_EQ(path.edges.size(), std::size_t{6});
    auto state = blankState();
    for (auto edge : path.edges) placeRoad(state, edge, PlayerId::One);
    state.awards.longestRoad = PlayerId::One;
    state.awards.roadLengths = {6, 0};
    const auto middle = path.vertices[3];
    EdgeId opponentEdge{};
    bool found = false;
    for (auto edge : topology.vertices[middle.value].edges) {
        if (!edge || std::find(path.edges.begin(), path.edges.end(), *edge) != path.edges.end()) continue;
        opponentEdge = *edge;
        found = true;
    }
    REQUIRE(found);
    placeRoad(state, opponentEdge, PlayerId::Two);
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    setResources(state, PlayerId::Two, SettlementCost);
    Game game(state);
    accepted(game, BuildSettlement{middle});
    CHECK_EQ(game.state().awards.roadLengths[0], std::uint8_t{3});
    CHECK(!game.state().awards.longestRoad);
}
