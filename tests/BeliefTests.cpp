#include "TestHelpers.h"
#include "catan/Belief.h"

using namespace catan;
using namespace test_support;

namespace {

void observeStep(Game& game, const Action& action,
                 DevelopmentBelief& first, DevelopmentBelief& second) {
    const auto before = game.state();
    accepted(game, action);
    first.observe(before, action, game.state());
    second.observe(before, action, game.state());
}

} // namespace

TEST(development_beliefs_split_only_for_the_player_who_cannot_see_the_draw) {
    auto state = blankState();
    state.developmentDeck = DevelopmentDeckState{};
    setResources(state, PlayerId::One, DevelopmentCost);
    setResources(state, PlayerId::Two, DevelopmentCost);
    Game game(state);
    DevelopmentBelief p1(PlayerId::One, state);
    DevelopmentBelief p2(PlayerId::Two, state);

    observeStep(game, BuyDevelopmentCard{}, p1, p2);
    observeStep(game, DrawDevelopmentCard{DevelopmentCard::Knight}, p1, p2);

    CHECK_EQ(p1.worldCount(), std::size_t{1});
    CHECK_EQ(p2.worldCount(), std::size_t{5});
    CHECK_NEAR(p2.probabilityOpponentHas(DevelopmentCard::Knight), 14.0 / 25.0, 1e-12);
    CHECK_NEAR(p2.probabilityOpponentHas(DevelopmentCard::VictoryPoint), 5.0 / 25.0, 1e-12);
    CHECK_NEAR(p2.expectedDeckCards(DevelopmentCard::Knight), 14.0 - 14.0 / 25.0, 1e-12);

    const auto p1State = p1.determinize(game.state(), 0.5);
    CHECK_EQ(p1State.players[index(PlayerId::One)].developmentCards.cards[index(DevelopmentCard::Knight)],
             std::uint8_t{1});
    CHECK(p2.contains(p2.determinize(game.state(), 0.37)));
}

TEST(own_draw_bayes_updates_the_opponent_hand_and_remaining_deck_jointly) {
    auto state = blankState();
    state.developmentDeck = DevelopmentDeckState{};
    setResources(state, PlayerId::One, DevelopmentCost);
    setResources(state, PlayerId::Two, DevelopmentCost);
    Game game(state);
    DevelopmentBelief p1(PlayerId::One, state);
    DevelopmentBelief p2(PlayerId::Two, state);

    observeStep(game, BuyDevelopmentCard{}, p1, p2);
    observeStep(game, DrawDevelopmentCard{DevelopmentCard::Knight}, p1, p2);
    observeStep(game, EndTurn{}, p1, p2);
    observeStep(game, BeginRoll{}, p1, p2);
    observeStep(game, RollDice{2}, p1, p2);
    observeStep(game, BuyDevelopmentCard{}, p1, p2);
    observeStep(game, DrawDevelopmentCard{DevelopmentCard::Knight}, p1, p2);

    CHECK_EQ(p1.worldCount(), std::size_t{5});
    CHECK_EQ(p2.worldCount(), std::size_t{5});
    CHECK_NEAR(p1.probabilityOpponentHas(DevelopmentCard::Knight), 13.0 / 24.0, 1e-12);
    CHECK_NEAR(p2.probabilityOpponentHas(DevelopmentCard::Knight), 13.0 / 24.0, 1e-12);
    CHECK_NEAR(p2.probabilityOpponentHas(DevelopmentCard::VictoryPoint), 5.0 / 24.0, 1e-12);
    for (const auto& hypothesis : p2.hypotheses())
        CHECK_EQ(hypothesis.hands[index(PlayerId::Two)].cards[index(DevelopmentCard::Knight)],
                 std::uint8_t{1});

    observeStep(game, EndTurn{}, p1, p2);
    observeStep(game, PlayKnight{}, p1, p2);
    CHECK_NEAR(p2.probabilityOpponentHas(DevelopmentCard::Knight), 0.0, 1e-12);
    CHECK_EQ(p2.worldCount(), std::size_t{1});
}
