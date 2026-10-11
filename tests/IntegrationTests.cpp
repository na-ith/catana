#include "TestHelpers.h"

#include <random>

using namespace catan;
using namespace test_support;

TEST(turn_phase_machine_requires_roll_before_main_actions) {
    Game game(blankState(Phase::AwaitRoll));
    CHECK(hasAction<BeginRoll>(game));
    CHECK(!hasAction<EndTurn>(game));
    CHECK(!game.transform(EndTurn{}).accepted);
    accepted(game, BeginRoll{});
    CHECK_EQ(game.state().turn.phase, Phase::DiceRoll);
    CHECK(!hasAction<EndTurn>(game));
    accepted(game, RollDice{6});
    CHECK_EQ(game.state().turn.phase, Phase::Main);
    CHECK(hasAction<EndTurn>(game));
}

TEST(ending_turn_switches_player_increments_turn_and_resets_turn_flags) {
    auto state = blankState();
    state.turnNumber = 8;
    state.turn.playedDevelopmentCard = true;
    state.players[0].developmentCards.cards[index(DevelopmentCard::Knight)] = 1;
    state.players[0].developmentCards.boughtThisTurn[index(DevelopmentCard::Knight)] = 1;
    Game game(state);
    accepted(game, EndTurn{});
    CHECK_EQ(game.state().activePlayer, PlayerId::Two);
    CHECK_EQ(game.decisionPlayer(), PlayerId::Two);
    CHECK_EQ(game.state().turnNumber, std::uint32_t{9});
    CHECK_EQ(game.state().turn.phase, Phase::AwaitRoll);
    CHECK(!game.state().turn.playedDevelopmentCard);
    CHECK_EQ(game.state().players[0].developmentCards.boughtThisTurn[index(DevelopmentCard::Knight)], std::uint8_t{0});
}

TEST(illegal_action_is_rejected_without_mutating_state) {
    auto state = blankState(Phase::AwaitRoll);
    setResources(state, PlayerId::One, resources(1, 1, 0, 0, 0));
    const auto resourcesBefore = state.players[0].resources;
    const auto bankBefore = state.bank.cards;
    const auto phaseBefore = state.turn.phase;
    Game game(state);
    CHECK(!game.transform(BuildRoad{EdgeId{0}}).accepted);
    CHECK_EQ(game.state().players[0].resources, resourcesBefore);
    CHECK_EQ(game.state().bank.cards, bankBefore);
    CHECK_EQ(game.state().turn.phase, phaseBefore);
    CHECK(!game.state().board.roads[0]);
}

TEST(state_invariant_failure_rolls_back_an_otherwise_legal_transition) {
    auto state = blankState();
    state.bank.cards[index(Resource::Ore)] = 18; // deliberately invalid total
    Game game(state);
    const auto beforePlayer = game.state().activePlayer;
    const auto result = game.transform(EndTurn{});
    CHECK(!result.accepted);
    CHECK_EQ(game.state().activePlayer, beforePlayer);
    CHECK_EQ(game.state().turn.phase, Phase::Main);
}

TEST(resource_conservation_holds_across_setup_rolls_builds_trades_and_discards) {
    Game game(108);
    finishPlacements(game);
    for (unsigned turn = 0; turn < 20; ++turn) {
        accepted(game, BeginRoll{});
        const auto outcomes = game.chanceOutcomes();
        REQUIRE(!outcomes.empty());
        auto chosen = outcomes[turn % outcomes.size()].action;
        if (turn % 5 == 0) {
            for (const auto& outcome : outcomes)
                if (outcome.action == Action{RollDice{7}}) chosen = outcome.action;
        }
        accepted(game, chosen);
        if (game.state().turn.phase == Phase::Discarding) {
            ResolveDiscards joint{};
            for (std::size_t p = 0; p < PlayerCount; ++p) {
                const auto choices = game.discardChoices(static_cast<PlayerId>(p));
                if (!choices.empty()) joint.discarded[p] = std::get<DiscardResources>(choices.front()).cards;
            }
            accepted(game, joint);
        }
        if (game.state().turn.phase == Phase::MoveRobber)
            accepted(game, firstAction<MoveRobber>(game));
        if (game.state().turn.phase == Phase::RobberSteal)
            accepted(game, firstAction<StealResource>(game));
        REQUIRE_EQ(game.state().turn.phase, Phase::Main);
        accepted(game, EndTurn{});
        for (std::size_t resource = 0; resource < ResourceCount; ++resource) {
            const unsigned cards = game.state().bank.cards[resource] +
                game.state().players[0].resources[resource] + game.state().players[1].resources[resource];
            CHECK_EQ(cards, 19U);
        }
    }
}

TEST(every_chance_outcome_is_legal_and_probabilities_sum_to_one) {
    for (Phase phase : {Phase::DiceRoll, Phase::DevelopmentDraw, Phase::RobberSteal}) {
        auto state = blankState(phase);
        if (phase == Phase::RobberSteal) setResources(state, PlayerId::Two, resources(1, 1, 1, 1, 1));
        Game game(state);
        const auto legal = game.legalTransformations();
        const auto outcomes = game.chanceOutcomes();
        double totalProbability = 0.0;
        for (const auto& outcome : outcomes) {
            totalProbability += outcome.probability;
            CHECK(isChanceAction(outcome.action));
            CHECK(std::find(legal.begin(), legal.end(), outcome.action) != legal.end());
        }
        CHECK_NEAR(totalProbability, 1.0, 1e-12);
    }
}

TEST(deterministic_random_playouts_preserve_invariants_and_never_stall) {
    for (std::uint32_t seed = 1; seed <= 12; ++seed) {
        std::mt19937 random(seed * 7919U);
        Game game(seed);
        for (unsigned step = 0; step < 500 && game.state().turn.phase != Phase::Finished; ++step) {
            const auto legal = game.legalTransformations();
            REQUIRE(!legal.empty());
            std::uniform_int_distribution<std::size_t> choose(0, legal.size() - 1);
            const auto result = game.transform(legal[choose(random)]);
            REQUIRE(result.accepted);
            std::string reason;
            CHECK(validateState(game.state(), &reason));
            const auto outcomes = game.chanceOutcomes();
            if (!outcomes.empty()) {
                double probability = 0.0;
                for (const auto& outcome : outcomes) probability += outcome.probability;
                CHECK_NEAR(probability, 1.0, 1e-10);
            }
        }
    }
}
