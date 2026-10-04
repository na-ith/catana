#include "TestHelpers.h"

#include <numeric>

using namespace catan;
using namespace test_support;

namespace {

HexId targetableHex(const GameState& state) {
    for (std::size_t h = 0; h < BoardTopology::HexCount; ++h)
        if (h != state.board.robberHex.value) return HexId{static_cast<std::uint8_t>(h)};
    throw std::runtime_error("no robber target");
}

Game gameReadyToRollWithHands(const Resources& first, const Resources& second) {
    auto state = blankState(Phase::AwaitRoll, 14);
    setResources(state, PlayerId::One, first);
    setResources(state, PlayerId::Two, second);
    return Game(state);
}

} // namespace

TEST(ranked_mode_has_exactly_two_players) {
    CHECK_EQ(PlayerCount, std::size_t{2});
    CHECK_EQ(opponent(PlayerId::One), PlayerId::Two);
    CHECK_EQ(opponent(PlayerId::Two), PlayerId::One);
}

TEST(ranked_discard_limit_allows_nine_cards) {
    auto game = gameReadyToRollWithHands(resources(9, 0, 0, 0, 0), resources(0, 0, 0, 0, 0));
    roll(game, 7);
    CHECK_EQ(game.state().turn.phase, Phase::MoveRobber);
    CHECK_EQ(game.state().turn.pendingDiscards[0], std::uint8_t{0});
}

TEST(ranked_discard_limit_discards_floor_half_above_nine) {
    for (std::uint8_t cards : {std::uint8_t{10}, std::uint8_t{11}, std::uint8_t{13}}) {
        auto game = gameReadyToRollWithHands(resources(cards, 0, 0, 0, 0), resources(0, 0, 0, 0, 0));
        roll(game, 7);
        CHECK_EQ(game.state().turn.phase, Phase::Discarding);
        CHECK_EQ(game.state().turn.pendingDiscards[0], static_cast<std::uint8_t>(cards / 2));
        CHECK_EQ(game.decisionPlayer(), PlayerId::One);
    }
}

TEST(both_players_discard_before_the_roller_moves_the_robber) {
    auto game = gameReadyToRollWithHands(resources(10, 0, 0, 0, 0), resources(0, 11, 0, 0, 0));
    roll(game, 7);
    CHECK_EQ(game.decisionPlayer(), PlayerId::One);
    const auto before = game.state();
    CHECK(!game.transform(DiscardResources{resources(5, 0, 0, 0, 0)}).accepted);
    CHECK_EQ(game.state(), before);
    accepted(game, ResolveDiscards{{resources(5, 0, 0, 0, 0), resources(0, 5, 0, 0, 0)}});
    CHECK_EQ(game.state().turn.phase, Phase::MoveRobber);
    CHECK_EQ(game.decisionPlayer(), PlayerId::One);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Lumber)], std::uint8_t{5});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Brick)], std::uint8_t{6});
}

TEST(discard_must_have_the_exact_required_size_and_owned_cards) {
    auto game = gameReadyToRollWithHands(resources(6, 4, 0, 0, 0), resources(0, 0, 0, 0, 0));
    roll(game, 7);
    CHECK(!game.transform(DiscardResources{resources(4, 0, 0, 0, 0)}).accepted);
    CHECK(!game.transform(DiscardResources{resources(6, 0, 0, 0, 0)}).accepted);
    CHECK(!game.transform(DiscardResources{resources(0, 5, 0, 0, 0)}).accepted);
    CHECK(game.transform(DiscardResources{resources(3, 2, 0, 0, 0)}).accepted);
}

TEST(joint_discard_validation_is_atomic_and_checks_both_hands) {
    auto game = gameReadyToRollWithHands(resources(6, 4, 0, 0, 0), resources(0, 0, 6, 4, 1));
    roll(game, 7);
    const auto before = game.state();
    for (const auto& invalid : std::vector<ResolveDiscards>{
             {{resources(3, 2, 0, 0, 0), resources(0, 0, 4, 0, 0)}},
             {{resources(3, 2, 0, 0, 0), resources(0, 0, 0, 5, 0)}},
             {{resources(0, 5, 0, 0, 0), resources(0, 0, 3, 2, 0)}}}) {
        CHECK(!game.transform(invalid).accepted);
        CHECK_EQ(game.state(), before);
    }
    CHECK_EQ(game.discardChoices(PlayerId::One).size(), std::size_t{5});
    CHECK(!game.discardChoices(PlayerId::Two).empty());
    CHECK_EQ(game.state(), before);
    accepted(game, ResolveDiscards{{resources(3, 2, 0, 0, 0), resources(0, 0, 3, 2, 0)}});
    CHECK_EQ(game.state().players[0].resources, resources(3, 2, 0, 0, 0));
    CHECK_EQ(game.state().players[1].resources, resources(0, 0, 3, 2, 1));
    CHECK_EQ(game.state().turn.pendingDiscards, (std::array<std::uint8_t, 2>{0, 0}));
    for (std::size_t r = 0; r < ResourceCount; ++r)
        CHECK_EQ(game.state().bank.cards[r], before.bank.cards[r] + before.players[0].resources[r]
                 - game.state().players[0].resources[r] + before.players[1].resources[r]
                 - game.state().players[1].resources[r]);
    const auto resolved = game.state();
    CHECK(!game.transform(ResolveDiscards{}).accepted);
    CHECK_EQ(game.state(), resolved);
}

TEST(joint_discard_requires_zero_for_player_not_discarding_and_returns_to_active_roller) {
    auto game = gameReadyToRollWithHands(resources(9, 0, 0, 0, 0), resources(0, 11, 0, 0, 0));
    auto state = game.state();
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    game = Game(state);
    roll(game, 7);
    const auto before = game.state();
    CHECK(!game.transform(ResolveDiscards{{resources(1, 0, 0, 0, 0), resources(0, 5, 0, 0, 0)}}).accepted);
    CHECK_EQ(game.state(), before);
    CHECK(game.discardChoices(PlayerId::One).empty());
    accepted(game, ResolveDiscards{{Resources{}, resources(0, 5, 0, 0, 0)}});
    CHECK_EQ(game.state().turn.phase, Phase::MoveRobber);
    CHECK_EQ(game.decisionPlayer(), PlayerId::Two);
    CHECK_EQ(game.state().players[0].resources, before.players[0].resources);
}

TEST(friendly_robber_protects_two_public_points_and_ignores_hidden_vp_cards) {
    auto state = blankState();
    const auto target = targetableHex(state);
    const auto& targetVertices = BoardTopology::standard().hexes[target.value].vertices;
    placeBuilding(state, targetVertices[0], PlayerId::Two);
    placeBuilding(state, targetVertices[2], PlayerId::Two);
    state.players[1].developmentCards.cards[index(DevelopmentCard::VictoryPoint)] = 3;
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{2});
    CHECK_EQ(victoryPoints(state, PlayerId::Two), std::uint8_t{5});
    CHECK(!robberMayTarget(state, PlayerId::One, target));

    placeBuilding(state, targetVertices[4], PlayerId::Two);
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{3});
    CHECK(robberMayTarget(state, PlayerId::One, target));
}

TEST(friendly_robber_counts_public_awards) {
    auto state = blankState();
    const auto target = targetableHex(state);
    placeBuilding(state, BoardTopology::standard().hexes[target.value].vertices[0], PlayerId::Two);
    state.awards.longestRoad = PlayerId::Two;
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{3});
    CHECK(robberMayTarget(state, PlayerId::One, target));
}

TEST(friendly_robber_protects_a_two_point_city_but_does_not_protect_the_mover) {
    auto state = blankState();
    const auto target = targetableHex(state);
    const auto vertices = BoardTopology::standard().hexes[target.value].vertices;

    placeBuilding(state, vertices[0], PlayerId::Two, BuildingType::City);
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{2});
    CHECK(!robberMayTarget(state, PlayerId::One, target));

    state.board.buildings[vertices[0].value] = Building{PlayerId::One, BuildingType::City};
    CHECK(robberMayTarget(state, PlayerId::One, target));
}

TEST(friendly_robber_forbids_a_shared_hex_while_either_opponent_building_is_protected) {
    auto state = blankState();
    const auto target = targetableHex(state);
    const auto vertices = BoardTopology::standard().hexes[target.value].vertices;

    placeBuilding(state, vertices[0], PlayerId::One);
    placeBuilding(state, vertices[2], PlayerId::Two);
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{1});
    CHECK(!robberMayTarget(state, PlayerId::One, target));

    placeBuilding(state, vertices[4], PlayerId::Two, BuildingType::City);
    CHECK_EQ(publicVictoryPoints(state, PlayerId::Two), std::uint8_t{3});
    CHECK(robberMayTarget(state, PlayerId::One, target));
}

TEST(friendly_robber_applies_to_knights_as_well_as_rolled_sevens) {
    auto state = blankState(Phase::AwaitRoll);
    const auto protectedHex = targetableHex(state);
    const auto vertices = BoardTopology::standard().hexes[protectedHex.value].vertices;
    placeBuilding(state, vertices[0], PlayerId::Two, BuildingType::City);
    state.players[0].developmentCards.cards[index(DevelopmentCard::Knight)] = 1;
    --state.developmentDeck.cards[index(DevelopmentCard::Knight)];

    Game game(state);
    accepted(game, PlayKnight{});
    CHECK_EQ(game.state().turn.phase, Phase::MoveRobber);
    for (const auto move : actions<MoveRobber>(game)) CHECK(move.hex != protectedHex);
}

TEST(robber_must_move_to_a_different_hex) {
    auto state = blankState(Phase::MoveRobber);
    state.turn.returnPhase = Phase::Main;
    Game game(state);
    CHECK(!game.transform(MoveRobber{state.board.robberHex}).accepted);
}

TEST(robber_move_creates_weighted_random_steal_then_transfers_one_card) {
    auto state = blankState(Phase::MoveRobber);
    state.turn.returnPhase = Phase::Main;
    const auto target = targetableHex(state);
    const auto vertices = BoardTopology::standard().hexes[target.value].vertices;
    placeBuilding(state, vertices[0], PlayerId::Two);
    placeBuilding(state, vertices[2], PlayerId::Two);
    placeBuilding(state, vertices[4], PlayerId::Two); // disable friendly robber
    setResources(state, PlayerId::Two, resources(2, 1, 0, 0, 0));
    Game game(state);

    accepted(game, MoveRobber{target});
    CHECK_EQ(game.state().turn.phase, Phase::RobberSteal);
    CHECK_NEAR(probabilityOf(game, StealResource{Resource::Lumber}), 2.0 / 3.0, 1e-12);
    CHECK_NEAR(probabilityOf(game, StealResource{Resource::Brick}), 1.0 / 3.0, 1e-12);
    CHECK_EQ(game.chanceOutcomes().size(), std::size_t{2});
    accepted(game, StealResource{Resource::Brick});
    CHECK_EQ(game.state().turn.phase, Phase::Main);
    CHECK_EQ(game.state().players[0].resources[index(Resource::Brick)], std::uint8_t{1});
    CHECK_EQ(game.state().players[1].resources[index(Resource::Brick)], std::uint8_t{0});
}

TEST(robber_does_not_steal_without_an_adjacent_victim_or_card) {
    auto state = blankState(Phase::MoveRobber);
    state.turn.returnPhase = Phase::Main;
    const auto target = targetableHex(state);
    Game game(state);
    accepted(game, MoveRobber{target});
    CHECK_EQ(game.state().turn.phase, Phase::Main);
    CHECK(game.chanceOutcomes().empty());
}

TEST(ranked_victory_requires_fifteen_points) {
    auto state = blankState(Phase::DevelopmentDraw);
    for (std::size_t i = 0; i < 4; ++i)
        placeBuilding(state, VertexId{static_cast<std::uint8_t>(i * 10)}, PlayerId::One, BuildingType::City);
    state.awards.longestRoad = PlayerId::One;
    state.awards.largestArmy = PlayerId::One;
    state.players[0].developmentCards.cards[index(DevelopmentCard::VictoryPoint)] = 2;
    state.developmentDeck.cards[index(DevelopmentCard::VictoryPoint)] = 3;
    CHECK_EQ(publicVictoryPoints(state, PlayerId::One), std::uint8_t{12});
    CHECK_EQ(victoryPoints(state, PlayerId::One), std::uint8_t{14});
    Game game(state);
    accepted(game, DrawDevelopmentCard{DevelopmentCard::VictoryPoint});
    CHECK_EQ(game.state().winner, std::optional<PlayerId>{PlayerId::One});
    CHECK_EQ(game.state().turn.phase, Phase::Finished);
    CHECK_EQ(victoryPoints(game.state(), PlayerId::One), std::uint8_t{15});
}

TEST(resignation_immediately_awards_the_other_player) {
    auto state = blankState(Phase::Main);
    state.turn.decisionPlayer = PlayerId::Two;
    state.activePlayer = PlayerId::Two;
    Game game(state);
    accepted(game, Resign{});
    CHECK_EQ(game.state().winner, std::optional<PlayerId>{PlayerId::One});
    CHECK_EQ(game.state().turn.phase, Phase::Finished);
    CHECK(game.legalTransformations().empty());
}
