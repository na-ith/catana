#include "TestHelpers.h"

using namespace catan;
using namespace test_support;

namespace {

double outcomeTotal(const Game& game) {
    double total = 0.0;
    for (const auto& outcome : game.chanceOutcomes()) total += outcome.probability;
    return total;
}

Game diceGame(GameState state) {
    state.turn.phase = Phase::DiceRoll;
    state.turn.decisionPlayer = state.activePlayer;
    return Game(state);
}

} // namespace

TEST(initial_balanced_dice_probabilities_match_two_six_sided_dice) {
    auto game = diceGame(blankState());
    CHECK_EQ(game.chanceOutcomes().size(), std::size_t{11});
    CHECK_NEAR(outcomeTotal(game), 1.0, 1e-12);
    constexpr std::array<unsigned, 13> ways{0, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};
    for (std::uint8_t sum = 2; sum <= 12; ++sum)
        CHECK_NEAR(probabilityOf(game, RollDice{sum}), ways[sum] / 36.0, 1e-12);
}

TEST(decision_to_roll_and_dice_chance_node_are_separate) {
    Game game(blankState(Phase::AwaitRoll));
    CHECK(game.chanceOutcomes().empty());
    CHECK(hasAction<BeginRoll>(game));
    accepted(game, BeginRoll{});
    REQUIRE_EQ(game.state().turn.phase, Phase::DiceRoll);
    for (const auto& action : game.legalTransformations()) CHECK(isChanceAction(action));
}

TEST(balanced_deck_draws_without_replacement) {
    auto game = diceGame(blankState());
    accepted(game, RollDice{6});
    CHECK_EQ(game.state().dice.cardsLeft, std::uint8_t{35});
    CHECK_EQ(game.state().dice.remainingBySum[6], std::uint8_t{4});
    CHECK_EQ(game.state().dice.recentRollCount, std::uint8_t{1});
    CHECK_EQ(game.state().dice.recentRolls[0], std::uint8_t{6});
    CHECK_EQ(game.state().dice.recentCounts[6], std::uint8_t{1});
}

TEST(recent_sum_probability_is_reduced_by_thirty_four_percent) {
    auto baseline = diceGame(blankState());
    auto recentState = blankState();
    recentState.dice.recentRolls[0] = 6;
    recentState.dice.recentRollCount = 1;
    recentState.dice.recentCounts[6] = 1;
    auto recent = diceGame(recentState);
    const double baselineOdds = probabilityOf(baseline, RollDice{6}) / probabilityOf(baseline, RollDice{5});
    const double recentOdds = probabilityOf(recent, RollDice{6}) / probabilityOf(recent, RollDice{5});
    CHECK_NEAR(recentOdds / baselineOdds, 0.66, 1e-12);
}

TEST(recent_roll_memory_keeps_only_the_last_five_sums) {
    Game game(blankState(Phase::AwaitRoll));
    for (auto sum : {2, 3, 4, 5, 6, 8}) {
        roll(game, static_cast<std::uint8_t>(sum));
        accepted(game, EndTurn{});
    }
    CHECK_EQ(game.state().dice.recentRollCount, std::uint8_t{5});
    CHECK_EQ(game.state().dice.recentCounts[2], std::uint8_t{0});
    for (auto sum : {3, 4, 5, 6, 8}) CHECK_EQ(game.state().dice.recentCounts[sum], std::uint8_t{1});
}

TEST(seven_streak_reduces_owners_odds_and_increases_opponents_odds) {
    auto firstState = blankState();
    firstState.dice.sevenStreakPlayer = PlayerId::One;
    firstState.dice.sevenStreakCount = 1;
    firstState.activePlayer = PlayerId::One;
    auto first = diceGame(firstState);
    const double firstOdds = probabilityOf(first, RollDice{7}) / probabilityOf(first, RollDice{6});

    auto secondState = firstState;
    secondState.activePlayer = PlayerId::Two;
    auto second = diceGame(secondState);
    const double secondOdds = probabilityOf(second, RollDice{7}) / probabilityOf(second, RollDice{6});
    const double ordinaryOdds = 6.0 / 5.0;
    CHECK_NEAR(firstOdds / ordinaryOdds, 0.6, 1e-12);
    CHECK_NEAR(secondOdds / ordinaryOdds, 1.4, 1e-12);
}

TEST(seven_imbalance_favours_the_player_with_fewer_sevens) {
    auto firstState = blankState();
    firstState.dice.sevensByPlayer = {3, 1};
    firstState.activePlayer = PlayerId::One;
    auto first = diceGame(firstState);
    const double firstOdds = probabilityOf(first, RollDice{7}) / probabilityOf(first, RollDice{6});

    auto secondState = firstState;
    secondState.activePlayer = PlayerId::Two;
    auto second = diceGame(secondState);
    const double secondOdds = probabilityOf(second, RollDice{7}) / probabilityOf(second, RollDice{6});
    const double ordinaryOdds = 6.0 / 5.0;
    CHECK_NEAR(firstOdds / ordinaryOdds, 0.5, 1e-12);
    CHECK_NEAR(secondOdds / ordinaryOdds, 1.5, 1e-12);
}

TEST(seven_adjustment_is_clamped_and_zero_probability_outcomes_are_not_legal) {
    auto state = blankState();
    state.dice.sevenStreakPlayer = PlayerId::One;
    state.dice.sevenStreakCount = 3;
    state.activePlayer = PlayerId::One;
    auto game = diceGame(state);
    CHECK_NEAR(probabilityOf(game, RollDice{7}), 0.0, 1e-12);
    bool legalSeven = false;
    for (const auto& action : game.legalTransformations()) legalSeven |= action == Action{RollDice{7}};
    CHECK(!legalSeven);
}

TEST(seven_adjustment_combines_ownership_and_streak_exactly_as_published) {
    for (const auto active : {PlayerId::One, PlayerId::Two}) {
        for (std::uint16_t first = 0; first <= 4; ++first) {
            for (std::uint16_t second = 0; second <= 4; ++second) {
                for (std::uint8_t streakCount = 0; streakCount <= 3; ++streakCount) {
                    auto state = blankState();
                    state.activePlayer = active;
                    state.dice.sevensByPlayer = {first, second};
                    if (streakCount) {
                        state.dice.sevenStreakPlayer = PlayerId::One;
                        state.dice.sevenStreakCount = streakCount;
                    }
                    auto game = diceGame(state);

                    const unsigned totalSevens = first + second;
                    double ownershipAdjustment = 1.0;
                    if (totalSevens >= 2) {
                        const auto activeSevens = active == PlayerId::One ? first : second;
                        ownershipAdjustment = 2.0 - 2.0 * activeSevens / totalSevens;
                    }
                    const double streakAdjustment = streakCount == 0 ? 0.0 :
                        0.4 * streakCount * (active == PlayerId::One ? -1.0 : 1.0);
                    const double expectedMultiplier = std::clamp(
                        ownershipAdjustment + streakAdjustment, 0.0, 2.0);
                    const double actualMultiplier = probabilityOf(game, RollDice{7}) == 0.0 ? 0.0 :
                        (probabilityOf(game, RollDice{7}) / probabilityOf(game, RollDice{6})) / (6.0 / 5.0);
                    CHECK_NEAR(actualMultiplier, expectedMultiplier, 1e-12);
                }
            }
        }
    }
}

TEST(seven_history_is_recorded_even_while_friendly_robber_is_active) {
    auto game = diceGame(blankState());
    accepted(game, RollDice{7});
    CHECK_EQ(game.state().dice.sevensByPlayer[0], std::uint16_t{1});
    CHECK_EQ(game.state().dice.sevenStreakPlayer, std::optional<PlayerId>{PlayerId::One});
    CHECK_EQ(game.state().dice.sevenStreakCount, std::uint8_t{1});
}

TEST(balanced_deck_keeps_the_thirteenth_draw_then_reshuffles_before_the_next_draw) {
    auto state = blankState();
    state.dice.remainingBySum.fill(0);
    state.dice.remainingBySum[2] = 1;
    state.dice.remainingBySum[3] = 2;
    state.dice.remainingBySum[4] = 3;
    state.dice.remainingBySum[5] = 4;
    state.dice.remainingBySum[6] = 3;
    state.dice.cardsLeft = 13;
    auto game = diceGame(state);
    accepted(game, RollDice{2});
    CHECK_EQ(game.state().dice.cardsLeft, std::uint8_t{12});
    CHECK_EQ(game.state().dice.remainingBySum[2], std::uint8_t{0});

    accepted(game, EndTurn{});
    accepted(game, BeginRoll{});
    CHECK_EQ(game.state().turn.phase, Phase::DiceRoll);
    CHECK_EQ(game.state().dice.cardsLeft, std::uint8_t{36});
    constexpr std::array<std::uint8_t, 13> standard{0, 0, 1, 2, 3, 4, 5, 6, 5, 4, 3, 2, 1};
    CHECK_EQ(game.state().dice.remainingBySum, standard);
}

TEST(restored_dice_chance_node_reshuffles_before_exposing_outcomes) {
    auto state = blankState(Phase::DiceRoll);
    state.dice.remainingBySum.fill(0);
    state.dice.remainingBySum[6] = 5;
    state.dice.remainingBySum[7] = 6;
    state.dice.remainingBySum[8] = 1;
    state.dice.cardsLeft = 12;

    Game game(state);
    CHECK_EQ(game.state().dice.cardsLeft, std::uint8_t{36});
    CHECK_NEAR(probabilityOf(game, RollDice{2}), 1.0 / 36.0, 1e-12);
    CHECK_NEAR(probabilityOf(game, RollDice{12}), 1.0 / 36.0, 1e-12);
}

TEST(all_chance_distributions_are_normalized) {
    auto dice = diceGame(blankState());
    CHECK_NEAR(outcomeTotal(dice), 1.0, 1e-12);

    auto drawState = blankState(Phase::DevelopmentDraw);
    Game draw(drawState);
    CHECK_NEAR(outcomeTotal(draw), 1.0, 1e-12);

    auto stealState = blankState(Phase::RobberSteal);
    setResources(stealState, PlayerId::Two, resources(1, 2, 3, 4, 5));
    Game steal(stealState);
    CHECK_NEAR(outcomeTotal(steal), 1.0, 1e-12);
}
