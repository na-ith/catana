#include "TestHelpers.h"
#include "catan/Mcts.h"
#include "catan/TradePolicy.h"

#include <set>

using namespace catan;
using namespace test_support;

namespace {
MaritimeTradeBatch batch(Resource give, Resource receive) { return {{{give, receive}}}; }
bool hasTrade(const MctsResult& result) {
    return std::any_of(result.actions.begin(), result.actions.end(), [](const auto& evaluation) {
        return std::holds_alternative<MaritimeTrade>(evaluation.action) ||
               std::holds_alternative<MaritimeTradeBatch>(evaluation.action);
    });
}
MctsConfig smallConfig() {
    MctsConfig config;
    config.iterations = 400;
    config.maxRolloutSteps = 0;
    return config;
}
}

TEST(trade_batch_is_atomic_and_identical_to_its_ordered_single_trades) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(4, 0, 4, 0, 0));
    setResources(state, PlayerId::Two, resources(0, 0, 15, 0, 0));
    const MaritimeTradeBatch ordered{{{Resource::Wool, Resource::Brick}, {Resource::Lumber, Resource::Wool}}};
    Game singles(state), combined(state);
    CHECK_EQ(state.bank.cards[index(Resource::Wool)], std::uint8_t{0});
    for (const auto& trade : ordered.trades) accepted(singles, trade);
    accepted(combined, ordered);
    CHECK_EQ(combined.state(), singles.state());
    CHECK(validateState(combined.state()));
    Game invalid(state);
    CHECK(!invalid.transform(MaritimeTradeBatch{{ordered.trades[1], ordered.trades[0]}}).accepted);
    CHECK_EQ(invalid.state(), state);
    CHECK(!invalid.transform(MaritimeTradeBatch{}).accepted);
    CHECK(!invalid.transform(MaritimeTradeBatch{{{Resource::Wool, Resource::Brick}, {Resource::Ore, Resource::Grain}}}).accepted);
    CHECK_EQ(invalid.state(), state); // First leg must not leak on later rejection.
    CHECK(!invalid.transform(batch(Resource::Lumber, Resource::Lumber)).accepted);
    CHECK(!invalid.transform(batch(static_cast<Resource>(99), Resource::Ore)).accepted);
    CHECK_EQ(invalid.state(), state);
    auto corrupt = state;
    --corrupt.bank.cards[index(Resource::Ore)];
    Game corruptGame(corrupt);
    CHECK(!corruptGame.transform(batch(Resource::Wool, Resource::Brick)).accepted);
    CHECK_EQ(corruptGame.state(), corrupt);
}

TEST(trade_batches_keep_one_sequence_per_result_and_preserve_stopping_points) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(8, 0, 0, 0, 0));
    const auto options = search::tradeOptions(state, 512);
    REQUIRE(options.complete);
    std::set<Resources> hands;
    bool oneTrade = false, twoTrades = false;
    unsigned brickWool = 0;
    for (const auto& action : options.batches) {
        const auto& trades = std::get<MaritimeTradeBatch>(action).trades;
        Game game(state);
        accepted(game, action);
        CHECK(hands.insert(game.state().players[0].resources).second);
        oneTrade |= trades.size() == 1;
        twoTrades |= trades.size() == 2;
        if (game.state().players[0].resources == resources(0, 1, 1, 0, 0)) ++brickWool;
    }
    CHECK(oneTrade);
    CHECK(twoTrades);
    CHECK_EQ(brickWool, 1U); // Brick then wool and wool then brick share one edge.
    CHECK_EQ(options.batches.size(), std::size_t{14}); // 20 ordered trade prefixes -> 14 results.
    const auto fallback = search::tradeOptions(state, 2);
    CHECK(!fallback.complete);
    CHECK(fallback.batches.empty()); // Never return a silently incomplete set.
    auto config = smallConfig();
    config.maxTradeBatchStates = 2;
    MctsSession session(state, config);
    CHECK(hasTrade(session.result()));
    const auto fallbackResult = session.result();
    CHECK(std::any_of(fallbackResult.actions.begin(), fallbackResult.actions.end(), [](const auto& item) {
        return std::holds_alternative<MaritimeTrade>(item.action);
    }));
}

TEST(mcts_batches_are_observer_visible_executable_and_reusable_without_consecutive_batches) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(8, 0, 0, 0, 0));
    InformationMctsSession session(InformationState(state, PlayerId::One), smallConfig());
    const auto result = session.run();
    REQUIRE(hasTrade(result));
    const MaritimeTradeBatch selected{{{Resource::Lumber, Resource::Brick}, {Resource::Lumber, Resource::Wool}}};
    Game expected(state);
    accepted(expected, selected);
    const auto found = std::find_if(result.actions.begin(), result.actions.end(), [&](const auto& evaluation) {
        Game candidate(state);
        return std::holds_alternative<MaritimeTradeBatch>(evaluation.action) &&
               candidate.transform(evaluation.action).accepted && candidate.state() == expected.state();
    });
    REQUIRE(found != result.actions.end());
    CHECK(found->visits > 0);
    CHECK(session.transform(found->action).accepted);
    CHECK_EQ(session.state().state().players[0].resources, expected.state().players[0].resources);
    CHECK(session.result().iterations > 0);
    CHECK(!hasTrade(session.result()));
    MctsSession aliases(state, smallConfig());
    aliases.run();
    CHECK(aliases.advance(MaritimeTradeBatch{{{Resource::Lumber, Resource::Wool}, {Resource::Lumber, Resource::Brick}}}));
    CHECK_EQ(aliases.state(), expected.state()); // Another ordering retains the canonical child.
    // The simulator still allows a normal second trade when affordable.
    auto rich = state;
    setResources(rich, PlayerId::One, resources(12, 0, 0, 0, 0));
    MctsSession raw(rich, smallConfig());
    raw.run();
    raw.advance(batch(Resource::Lumber, Resource::Brick));
    CHECK(!hasTrade(raw.result()));
    CHECK(Game(raw.state()).canTradeBatch(batch(Resource::Lumber, Resource::Ore)));
    CHECK(!raw.advance(MaritimeTrade{Resource::Lumber, Resource::Ore}));
    CHECK(hasTrade(raw.result())); // Manual observation safely resets the path.
}

TEST(trade_batch_observation_preserves_all_hidden_development_worlds) {
    auto state = blankState();
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    setResources(state, PlayerId::One, resources(8, 0, 0, 0, 0));
    setResources(state, PlayerId::Two, DevelopmentCost);
    InformationState info(state, PlayerId::One);
    REQUIRE(info.transform(BuyDevelopmentCard{}).accepted);
    REQUIRE(info.transform(DrawUnknownDevelopmentCard{}).accepted);
    REQUIRE(info.transform(EndTurn{}).accepted);
    REQUIRE(info.transform(BeginRoll{}).accepted);
    REQUIRE(info.transform(RollDice{2}).accepted);
    const auto before = info;
    const MaritimeTradeBatch trades{{{Resource::Lumber, Resource::Brick}, {Resource::Lumber, Resource::Wool}}};
    REQUIRE(info.transform(trades).accepted);
    CHECK_EQ(info.worldCount(), std::size_t{5});
    for (const auto& world : before.worlds()) {
        Game expected(world.state);
        accepted(expected, trades);
        const auto found = std::find_if(info.worlds().begin(), info.worlds().end(), [&](const auto& next) {
            return next.state == expected.state();
        });
        REQUIRE(found != info.worlds().end());
        CHECK_NEAR(found->probability, world.probability, 1e-12);
    }
}

TEST(build_then_trade_is_removed_only_when_the_full_reordered_sequence_is_equivalent) {
    auto state = blankState();
    const auto path = simplePath(3);
    placeBuilding(state, path.vertices[0], PlayerId::One);
    setResources(state, PlayerId::One, resources(10, 2, 0, 0, 0));
    Game game(state);
    const Action first = BuildRoad{path.edges[0]};
    auto context = search::nextTradeContext(state, nullptr, first);
    accepted(game, first);
    const Action second = BuildRoad{path.edges[1]};
    context = search::nextTradeContext(game.state(), &*context, second);
    accepted(game, second);
    CHECK(search::redundantTrade(game.state(), &*context, batch(Resource::Lumber, Resource::Ore)));
    MctsSession session(state, smallConfig());
    session.advance(first);
    session.advance(second);
    CHECK(!hasTrade(session.result()));
    // Rules are not pruned: the same trade remains legal for a real player.
    accepted(game, MaritimeTrade{Resource::Lumber, Resource::Ore});

    auto beforeTrades = state;
    setResources(beforeTrades, PlayerId::One, resources(14, 2, 0, 0, 0));
    const auto firstTrade = batch(Resource::Lumber, Resource::Wool);
    auto traded = search::nextTradeContext(beforeTrades, nullptr, firstTrade);
    Game mixed(beforeTrades);
    accepted(mixed, firstTrade);
    traded = search::nextTradeContext(mixed.state(), &*traded, first);
    accepted(mixed, first);
    CHECK(search::redundantTrade(mixed.state(), &*traded, batch(Resource::Lumber, Resource::Ore)));
}

TEST(build_then_trade_preserves_bank_replenishment_and_new_port_rates) {
    auto state = blankState();
    const auto path = simplePath(2);
    placeBuilding(state, path.vertices[0], PlayerId::One);
    setResources(state, PlayerId::One, resources(6, 1, 0, 0, 0));
    setResources(state, PlayerId::Two, resources(0, 18, 0, 0, 0));
    const Action build = BuildRoad{path.edges[0]};
    auto context = search::nextTradeContext(state, nullptr, build);
    Game after(state);
    accepted(after, build);
    const auto trade = batch(Resource::Lumber, Resource::Brick);
    CHECK(!Game(state).canTradeBatch(trade));
    CHECK(after.canTradeBatch(trade));
    CHECK(!search::redundantTrade(after.state(), &*context, trade));
    MctsSession bankSession(state, smallConfig());
    bankSession.advance(build);
    CHECK(hasTrade(bankSession.result()));

    auto portState = blankState();
    portState.board.ports[0].type = PortType::Lumber;
    const auto edge = portState.board.ports[0].edge;
    const auto vertex = BoardTopology::standard().edges[edge.value].vertices[0];
    placeRoad(portState, edge, PlayerId::One);
    setResources(portState, PlayerId::One, resources(5, 1, 1, 1, 0));
    const Action settlement = BuildSettlement{vertex};
    auto portContext = search::nextTradeContext(portState, nullptr, settlement);
    Game portGame(portState);
    accepted(portGame, settlement);
    CHECK_EQ(maritimeRate(portGame.state(), PlayerId::One, Resource::Lumber), std::uint8_t{2});
    CHECK(!search::redundantTrade(portGame.state(), &*portContext, batch(Resource::Lumber, Resource::Ore)));
    MctsSession portSession(portState, smallConfig());
    portSession.advance(settlement);
    CHECK(hasTrade(portSession.result()));
}

TEST(development_purchases_plays_and_turn_changes_reset_trade_ordering) {
    auto state = blankState();
    setResources(state, PlayerId::One, resources(8, 0, 2, 2, 2));
    state.players[0].developmentCards.cards[index(DevelopmentCard::Monopoly)] = 1;
    --state.developmentDeck.cards[index(DevelopmentCard::Monopoly)];
    const auto first = batch(Resource::Lumber, Resource::Brick);
    auto context = search::nextTradeContext(state, nullptr, first);
    Game game(state);
    accepted(game, first);
    CHECK(search::redundantTrade(game.state(), &*context, batch(Resource::Lumber, Resource::Ore)));
    for (const auto& boundary : std::vector<Action>{BuyDevelopmentCard{}, PlayKnight{}, PlayMonopoly{Resource::Grain},
             PlayYearOfPlenty{}, PlayRoadBuilding{}, EndTurn{}, DrawDevelopmentCard{DevelopmentCard::Knight}})
        CHECK(!search::nextTradeContext(game.state(), &*context, boundary));
    MctsSession session(state, smallConfig());
    session.run();
    session.advance(first);
    CHECK(!hasTrade(session.result()));
    session.advance(BuyDevelopmentCard{});
    session.advance(DrawDevelopmentCard{DevelopmentCard::Knight});
    CHECK(hasTrade(session.result()));
    session.advance(PlayMonopoly{Resource::Grain});
    CHECK(hasTrade(session.result()));
}
