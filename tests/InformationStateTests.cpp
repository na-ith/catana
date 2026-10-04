#include "TestHelpers.h"
#include "catan/InformationState.h"
#include "catan/Mcts.h"

using namespace catan;
using namespace test_support;

TEST(information_joint_discards_preserve_hidden_card_beliefs_for_both_observers) {
    for (auto observer : {PlayerId::One, PlayerId::Two}) {
        auto state = blankState();
        state.activePlayer = opponent(observer);
        state.turn.decisionPlayer = opponent(observer);
        setResources(state, observer, resources(6, 4, 0, 0, 0));
        setResources(state, opponent(observer), resources(0, 0, 7, 5, 1));
        InformationState info(state, observer);
        REQUIRE(info.transform(BuyDevelopmentCard{}).accepted);
        REQUIRE(info.transform(DrawUnknownDevelopmentCard{}).accepted);
        REQUIRE(info.transform(EndTurn{}).accepted);
        REQUIRE(info.transform(BeginRoll{}).accepted);
        REQUIRE(info.transform(RollDice{7}).accepted);
        const auto before = info;
        REQUIRE_EQ(before.worldCount(), std::size_t{5});
        CHECK_EQ(info.decisionPlayer(), observer);
        CHECK(!info.transform(DiscardResources{std::get<DiscardResources>(info.discardChoices(observer).front()).cards}).accepted);
        CHECK_EQ(info, before);
        ResolveDiscards joint{};
        joint.discarded[index(observer)] = resources(3, 2, 0, 0, 0);
        joint.discarded[index(opponent(observer))] = resources(0, 0, 3, 2, 0);
        const auto result = info.transform(joint);
        CHECK(result.accepted);
        CHECK_EQ(info.worldCount(), before.worldCount());
        CHECK_EQ(info.state().turn.phase, Phase::MoveRobber);
        CHECK_EQ(info.state().players[index(observer)].resources, resources(3, 2, 0, 0, 0));
        CHECK_EQ(info.state().players[index(opponent(observer))].resources, resources(0, 0, 3, 2, 0));
        for (const auto& world : before.worlds()) {
            Game expected(world.state);
            accepted(expected, joint);
            REQUIRE(info.contains(expected.state()));
            const auto found = std::find_if(info.worlds().begin(), info.worlds().end(),
                [&](const auto& next) { return next.state == expected.state(); });
            REQUIRE(found != info.worlds().end());
            CHECK_NEAR(found->probability, world.probability, 1e-12);
        }
    }
}

namespace {
void apply(InformationState& state, const Action& action) {
    const auto result = state.transform(action);
    REQUIRE(result.accepted);
}
GameState developmentPosition(PlayerId active) {
    auto state = blankState();
    state.activePlayer = active;
    state.turn.decisionPlayer = active;
    setResources(state, PlayerId::One, resources(0, 0, 3, 3, 3));
    setResources(state, PlayerId::Two, resources(0, 0, 3, 3, 3));
    return state;
}
}

TEST(information_hidden_draw_has_no_authoritative_card_and_preserves_exact_probabilities) {
    InformationState info(developmentPosition(PlayerId::Two), PlayerId::One);
    apply(info, BuyDevelopmentCard{});
    REQUIRE_EQ(info.legalTransformations(), std::vector<Action>{DrawUnknownDevelopmentCard{}});
    REQUIRE_EQ(info.chanceOutcomes().size(), std::size_t{1});
    CHECK_NEAR(info.chanceOutcomes()[0].probability, 1.0, 1e-12);
    const auto before = info;
    CHECK(!info.transform(DrawDevelopmentCard{DevelopmentCard::Knight}).accepted);
    CHECK_EQ(info, before);
    apply(info, DrawUnknownDevelopmentCard{});
    CHECK_EQ(info.worldCount(), std::size_t{5});
    CHECK_EQ(info.opponentCardCount(), 1U);
    CHECK_EQ(info.deckCardCount(), 24U);
    CHECK_EQ(info.state().turn.phase, Phase::Main);
    CHECK_EQ(info.state().players[1].developmentCards, DevelopmentHand{});
    CHECK_EQ(info.state().developmentDeck.cards, (std::array<std::uint8_t, DevelopmentCardTypeCount>{}));
    CHECK_NEAR(info.expectedOpponentCards(DevelopmentCard::Knight), 14.0 / 25.0, 1e-12);
    CHECK_NEAR(info.expectedDeckCards(DevelopmentCard::Knight), 14.0 - 14.0 / 25.0, 1e-12);
    for (const auto& world : info.worlds()) {
        std::string error;
        CHECK(validateState(world.state, &error));
        CHECK(info.contains(world.state));
    }
    // Newly purchased cards cannot explain an observed same-turn knight play.
    const auto after = info;
    CHECK(!info.transform(PlayKnight{}).accepted);
    CHECK_EQ(info, after);
    apply(info, BuyDevelopmentCard{});
    apply(info, DrawUnknownDevelopmentCard{});
    CHECK_EQ(info.worldCount(), std::size_t{15});
    CHECK_EQ(info.opponentCardCount(), 2U);
    double doubleKnight = 0.0;
    for (const auto& world : info.worlds())
        if (world.state.players[1].developmentCards.cards[index(DevelopmentCard::Knight)] == 2)
            doubleKnight += world.probability;
    CHECK_NEAR(doubleKnight, 14.0 / 25.0 * 13.0 / 24.0, 1e-12);
}

TEST(information_private_draw_and_public_play_condition_the_joint_hand_and_deck) {
    for (const auto observer : {PlayerId::One, PlayerId::Two}) {
        InformationState info(developmentPosition(opponent(observer)), observer);
        apply(info, BuyDevelopmentCard{});
        apply(info, DrawUnknownDevelopmentCard{});
        apply(info, EndTurn{});
        apply(info, BeginRoll{});
        apply(info, RollDice{2});
        apply(info, BuyDevelopmentCard{});
        double knightProbability = 0.0;
        for (const auto& outcome : info.chanceOutcomes())
            if (outcome.action == Action(DrawDevelopmentCard{DevelopmentCard::Knight}))
                knightProbability = outcome.probability;
        CHECK_NEAR(knightProbability, 14.0 / 25.0, 1e-12);
        CHECK(!info.transform(DrawUnknownDevelopmentCard{}).accepted);
        apply(info, DrawDevelopmentCard{DevelopmentCard::Knight});
        CHECK_EQ(info.worldCount(), std::size_t{5});
        CHECK_NEAR(info.expectedOpponentCards(DevelopmentCard::Knight), 13.0 / 24.0, 1e-12);
        CHECK_EQ(info.state().players[index(observer)].developmentCards.cards[index(DevelopmentCard::Knight)], std::uint8_t{1});
        apply(info, EndTurn{});
        apply(info, PlayKnight{});
        CHECK_EQ(info.worldCount(), std::size_t{1});
        CHECK_EQ(info.opponentCardCount(), 0U);
        CHECK_NEAR(info.expectedDeckCards(DevelopmentCard::Knight), 12.0, 1e-12);
        CHECK_EQ(info.state().turn.phase, Phase::MoveRobber);
    }
}

TEST(information_mcts_accepts_unknown_observations_and_keeps_known_subtrees) {
    MctsConfig config;
    config.iterations = 60;
    config.maxRolloutSteps = 0;
    InformationMctsSession tree(InformationState(developmentPosition(PlayerId::Two), PlayerId::One), config);
    REQUIRE(tree.transform(BuyDevelopmentCard{}).accepted);
    const auto chance = tree.run();
    CHECK_EQ(chance.perspective, PlayerId::One);
    CHECK(chance.chanceNode);
    REQUIRE_EQ(chance.actions.size(), std::size_t{1});
    CHECK(std::holds_alternative<DrawUnknownDevelopmentCard>(chance.actions[0].action));
    REQUIRE(tree.transform(DrawUnknownDevelopmentCard{}).accepted);
    CHECK_EQ(tree.state().worldCount(), std::size_t{5});
    CHECK_EQ(tree.run().perspective, PlayerId::One);
    REQUIRE(tree.transform(EndTurn{}).accepted);
    REQUIRE(tree.transform(BeginRoll{}).accepted);
    REQUIRE(tree.transform(RollDice{2}).accepted);
    const auto result = tree.run();
    REQUIRE(result.bestAction.has_value());
    auto next = tree.state();
    apply(next, *result.bestAction);
    CHECK(tree.synchronize(*result.bestAction, next));
    CHECK(tree.result().iterations > 0);
}

TEST(information_does_not_report_an_unobserved_hidden_victory) {
    auto state = developmentPosition(PlayerId::Two);
    // Fourteen public points; only a VP draw ends the game.
    for (std::uint8_t v = 0; v < 4; ++v) placeBuilding(state, VertexId{v}, PlayerId::Two, BuildingType::City);
    for (std::uint8_t v = 4; v < 8; ++v) placeBuilding(state, VertexId{v}, PlayerId::Two);
    state.awards.longestRoad = PlayerId::Two;
    InformationState info(state, PlayerId::One);
    apply(info, BuyDevelopmentCard{});
    apply(info, DrawUnknownDevelopmentCard{});
    CHECK(!info.state().winner);
    CHECK_EQ(info.state().turn.phase, Phase::Main);
    const auto before = info;
    CHECK(!info.transform(ObserveVictory{PlayerId::One}).accepted);
    CHECK_EQ(info, before);
    apply(info, ObserveVictory{PlayerId::Two});
    CHECK_EQ(info.state().winner, std::optional{PlayerId::Two});
    CHECK_EQ(info.worldCount(), std::size_t{1});
}
