#include "TestHelpers.h"
#include "catan/Mcts.h"

#include <stdexcept>
#include <stop_token>

using namespace catan;
using namespace test_support;

TEST(mcts_simultaneous_discards_search_own_choices_and_reuse_the_joint_outcome) {
    auto state = blankState(Phase::Discarding);
    state.turn.pendingDiscards = {5, 5};
    setResources(state, PlayerId::One, resources(6, 4, 0, 0, 0));
    setResources(state, PlayerId::Two, resources(0, 0, 6, 4, 0));
    MctsConfig config;
    config.iterations = 2000;
    config.maxRolloutSteps = 0;
    for (auto observer : {PlayerId::One, PlayerId::Two}) {
        InformationMctsSession session(InformationState(state, observer), config);
        const auto result = session.run();
        CHECK_EQ(result.rootPlayer, observer);
        CHECK(!result.chanceNode);
        CHECK_EQ(result.actions.size(), std::size_t{5});
        const auto choices = Game(state).discardChoices(observer);
        for (const auto& evaluation : result.actions) {
            CHECK(std::find(choices.begin(), choices.end(), evaluation.action) != choices.end());
            CHECK(evaluation.visits > 0);
            CHECK(!evaluation.chanceProbability.has_value());
        }
        CHECK_EQ(session.state().state().players[0].resources, state.players[0].resources);
        CHECK_EQ(session.state().state().players[1].resources, state.players[1].resources);
        CHECK(result.maximumDepth >= 2);
        CHECK(session.transform(ResolveDiscards{{resources(3, 2, 0, 0, 0), resources(0, 0, 3, 2, 0)}}).accepted);
        CHECK_EQ(session.state().state().turn.phase, Phase::MoveRobber);
        CHECK(session.result().iterations > 0);
    }
    MctsSession raw(state, PlayerId::Two, config);
    bool rejected = false;
    try { raw.advance(DiscardResources{resources(0, 0, 3, 2, 0)}); }
    catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
    CHECK_EQ(raw.state(), state);
}

namespace {

MctsConfig fastConfig(std::uint32_t seed = 123) {
    MctsConfig config;
    config.iterations = 400;
    config.maxRolloutSteps = 24;
    config.seed = seed;
    return config;
}

GameState immediateCityWinState() {
    auto state = blankState();
    placeBuilding(state, VertexId{0}, PlayerId::One, BuildingType::Settlement);
    placeBuilding(state, VertexId{10}, PlayerId::One, BuildingType::City);
    placeBuilding(state, VertexId{20}, PlayerId::One, BuildingType::City);
    placeBuilding(state, VertexId{30}, PlayerId::One, BuildingType::City);
    placeBuilding(state, VertexId{40}, PlayerId::One, BuildingType::Settlement);
    state.awards.longestRoad = PlayerId::One;
    state.awards.largestArmy = PlayerId::One;
    state.players[0].developmentCards.cards[index(DevelopmentCard::VictoryPoint)] = 2;
    state.developmentDeck.cards[index(DevelopmentCard::VictoryPoint)] = 3;
    setResources(state, PlayerId::One, CityCost);
    return state;
}

} // namespace

TEST(mcts_reports_every_legal_root_action_with_visits_and_a_probability) {
    const auto state = blankState();
    const Game game(state);
    auto legal = game.legalTransformations();
    const auto result = MctsEngine(fastConfig()).search(state);
    CHECK(!result.chanceNode);
    CHECK_EQ(result.actions.size(), legal.size());
    CHECK(result.uniqueRootChildren > 0);
    CHECK(result.uniqueRootChildren <= result.actions.size());
    REQUIRE(result.bestAction.has_value());
    for (const auto& evaluation : result.actions) {
        CHECK(std::find(legal.begin(), legal.end(), evaluation.action) != legal.end());
        CHECK(evaluation.visits > 0);
        CHECK(evaluation.estimatedWinProbability >= 0.0);
        CHECK(evaluation.estimatedWinProbability <= 1.0);
        CHECK(!evaluation.chanceProbability.has_value());
    }
}

TEST(mcts_prunes_low_production_setup_but_preserves_simulator_actions) {
    Game game(42);
    auto config = fastConfig(1700);
    config.maxRolloutSteps = 0;
    MctsSession session(game.state(), config);
    const auto result = session.run();
    const auto& topology = BoardTopology::standard();
    std::optional<Action> pruned;
    std::size_t kept = 0;
    for (const auto& action : game.legalTransformations()) {
        const auto* settlement = std::get_if<PlaceSettlement>(&action);
        if (!settlement) continue;
        unsigned productive = 0;
        for (const auto hex : topology.vertices[settlement->vertex.value].hexes)
            if (hex && producedResource(game.state().board.hexes[hex->value].terrain)) ++productive;
        const bool found = std::any_of(result.actions.begin(), result.actions.end(),
            [&](const auto& evaluation) { return evaluation.action == action; });
        CHECK_EQ(found, productive > 1);
        if (productive <= 1) pruned = action;
        else ++kept;
    }
    CHECK_EQ(result.actions.size(), kept);
    REQUIRE(pruned.has_value());
    CHECK(!session.advance(*pruned));
    accepted(game, *pruned);
    CHECK_EQ(session.state(), game.state());
}

TEST(mcts_robber_pruning_handles_friendly_mixed_and_empty_hand_targets) {
    auto state = blankState(Phase::MoveRobber);
    const auto& topology = BoardTopology::standard();
    const auto opponentVertex = topology.hexes[0].vertices[3];
    state.board.robberHex = HexId{18};
    placeBuilding(state, VertexId{0}, PlayerId::One);
    placeBuilding(state, opponentVertex, PlayerId::Two, BuildingType::City);
    auto config = fastConfig(1702);
    config.maxRolloutSteps = 0;
    for (const bool friendly : {true, false}) {
        auto position = state;
        if (!friendly) position.awards.largestArmy = PlayerId::Two;
        CHECK_EQ(publicVictoryPoints(position, PlayerId::Two) > 2, !friendly);
        MctsSession session(position, config);
        const auto result = session.run();
        std::size_t expected = 0;
        for (const auto& action : Game(position).legalTransformations()) {
            const auto* robber = std::get_if<MoveRobber>(&action);
            if (!robber) continue;
            bool self = false, other = false;
            for (const auto vertex : topology.hexes[robber->hex.value].vertices) {
                const auto& building = position.board.buildings[vertex.value];
                if (!building) continue;
                self |= building->owner == PlayerId::One;
                other |= building->owner == PlayerId::Two;
            }
            const bool keep = !(self && !other) && (friendly || other);
            const bool found = std::any_of(result.actions.begin(), result.actions.end(),
                [&](const auto& evaluation) { return evaluation.action == action; });
            CHECK_EQ(found, keep);
            expected += keep;
        }
        CHECK(expected > 0);
        CHECK_EQ(result.actions.size(), expected);
    }

    // With no opponent buildings, a public award can disable friendly
    // protection while every target is excluded; restore legal continuations.
    state.board.buildings[opponentVertex.value].reset();
    state.awards.largestArmy = PlayerId::Two;
    state.awards.longestRoad = PlayerId::Two;
    MctsSession fallback(state, config);
    CHECK_EQ(fallback.run().actions.size(), actions<MoveRobber>(Game(state)).size());
}

TEST(mcts_prunes_monopoly_choices_that_take_no_cards) {
    auto state = blankState();
    state.players[index(PlayerId::One)].developmentCards.cards[index(DevelopmentCard::Monopoly)] = 1;
    setResources(state, PlayerId::Two, resources(0, 0, 1, 1, 1));

    // P2 has neither lumber nor brick. Those choices consume the same card and
    // produce exactly the same complete GameState; resources P2 owns do not.
    Game lumber(state);
    Game brick(state);
    Game ore(state);
    accepted(lumber, PlayMonopoly{Resource::Lumber});
    accepted(brick, PlayMonopoly{Resource::Brick});
    accepted(ore, PlayMonopoly{Resource::Ore});
    CHECK_EQ(lumber.state(), brick.state());
    CHECK(lumber.state() != ore.state());

    auto config = fastConfig(1701);
    config.iterations = 20;
    config.maxRolloutSteps = 0;
    MctsSession session(state, config);
    const auto result = session.run();

    // The simulator keeps all five choices legal, but the search omits the two
    // that consume the card without transferring anything.
    CHECK_EQ(Game(state).legalTransformations().size(), std::size_t{6});
    CHECK_EQ(result.actions.size(), std::size_t{4});
    CHECK_EQ(result.uniqueRootChildren, std::size_t{4});
    std::size_t yieldingMonopolies = 0;
    for (const auto& evaluation : result.actions) {
        const auto* monopoly = std::get_if<PlayMonopoly>(&evaluation.action);
        if (!monopoly) continue;
        CHECK(monopoly->resource != Resource::Lumber);
        CHECK(monopoly->resource != Resource::Brick);
        ++yieldingMonopolies;
    }
    CHECK_EQ(yieldingMonopolies, std::size_t{3});

    // The debugging stepper can still apply a simulator-legal pruned action.
    // There is no subtree to reuse, so the session starts fresh at its result.
    CHECK(!session.advance(PlayMonopoly{Resource::Brick}));
    CHECK_EQ(session.state(), brick.state());
    CHECK_EQ(session.result().iterations, std::size_t{0});

    bool rejectedIllegalAction = false;
    try { session.advance(BuildCity{VertexId{0}}); }
    catch (const std::invalid_argument&) { rejectedIllegalAction = true; }
    CHECK(rejectedIllegalAction);
}

TEST(mcts_keeps_a_yielding_pre_roll_monopoly_for_the_seven_discard_buffer_case) {
    auto state = blankState(Phase::AwaitRoll);
    state.players[index(PlayerId::One)].developmentCards.cards[index(DevelopmentCard::Monopoly)] = 1;
    setResources(state, PlayerId::One, resources(0, 0, 5, 5, 0));
    setResources(state, PlayerId::Two, resources(9, 0, 0, 0, 0));

    Game withoutBuffer(state);
    accepted(withoutBuffer, BeginRoll{});
    accepted(withoutBuffer, RollDice{7});
    CHECK_EQ(withoutBuffer.state().turn.pendingDiscards[index(PlayerId::One)], std::uint8_t{5});

    Game withBuffer(state);
    accepted(withBuffer, PlayMonopoly{Resource::Lumber});
    accepted(withBuffer, BeginRoll{});
    accepted(withBuffer, RollDice{7});
    CHECK_EQ(withBuffer.state().turn.pendingDiscards[index(PlayerId::One)], std::uint8_t{9});
    accepted(withBuffer, DiscardResources{resources(9, 0, 0, 0, 0)});
    CHECK_EQ(withBuffer.state().players[index(PlayerId::One)].resources,
             resources(0, 0, 5, 5, 0));

    MctsSession session(state, fastConfig(1702));
    const auto result = session.result();
    CHECK(std::any_of(result.actions.begin(), result.actions.end(), [](const auto& evaluation) {
        const auto* monopoly = std::get_if<PlayMonopoly>(&evaluation.action);
        return monopoly && monopoly->resource == Resource::Lumber;
    }));
}

TEST(mcts_defers_pre_roll_road_building_even_when_it_can_claim_longest_road) {
    auto ordinary = blankState(Phase::AwaitRoll);
    const auto ordinaryPath = simplePath(2);
    placeBuilding(ordinary, ordinaryPath.vertices.front(), PlayerId::One);
    ordinary.players[0].developmentCards.cards[index(DevelopmentCard::RoadBuilding)] = 1;
    CHECK(hasAction<PlayRoadBuilding>(Game(ordinary)));
    MctsSession ordinarySearch(ordinary, fastConfig(1801));
    const auto ordinaryResult = ordinarySearch.result();
    CHECK(std::none_of(ordinaryResult.actions.begin(), ordinaryResult.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlayRoadBuilding>(evaluation.action);
        }));

    auto claiming = blankState(Phase::AwaitRoll);
    const auto claimingPath = simplePath(5);
    placeBuilding(claiming, claimingPath.vertices.front(), PlayerId::One);
    for (std::size_t edge = 0; edge < 3; ++edge)
        placeRoad(claiming, claimingPath.edges[edge], PlayerId::One);
    claiming.players[0].developmentCards.cards[index(DevelopmentCard::RoadBuilding)] = 1;

    MctsSession claimingSearch(claiming, fastConfig(1802));
    const auto claimingResult = claimingSearch.result();
    CHECK(std::none_of(claimingResult.actions.begin(), claimingResult.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlayRoadBuilding>(evaluation.action);
        }));

    Game claim(claiming);
    accepted(claim, PlayRoadBuilding{});
    accepted(claim, PlaceRoad{claimingPath.edges[3]});
    accepted(claim, PlaceRoad{claimingPath.edges[4]});
    CHECK_EQ(claim.state().awards.longestRoad, std::optional<PlayerId>{PlayerId::One});
    CHECK(!claim.state().winner.has_value());
}

TEST(mcts_defers_pre_roll_year_of_plenty_even_when_bank_cards_can_be_reserved) {
    auto abundant = blankState(Phase::AwaitRoll);
    abundant.players[0].developmentCards.cards[index(DevelopmentCard::YearOfPlenty)] = 1;
    CHECK(hasAction<PlayYearOfPlenty>(Game(abundant)));
    MctsSession abundantSearch(abundant, fastConfig(1803));
    const auto abundantResult = abundantSearch.result();
    CHECK(std::none_of(abundantResult.actions.begin(), abundantResult.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlayYearOfPlenty>(evaluation.action);
        }));

    abundant.turn.phase = Phase::Main;
    MctsSession afterRollSearch(abundant, fastConfig(1804));
    const auto afterRollResult = afterRollSearch.result();
    CHECK(std::any_of(afterRollResult.actions.begin(), afterRollResult.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlayYearOfPlenty>(evaluation.action);
        }));

    auto scarce = blankState(Phase::AwaitRoll);
    scarce.players[0].developmentCards.cards[index(DevelopmentCard::YearOfPlenty)] = 1;
    std::optional<HexId> mountain;
    for (std::size_t hex = 0; hex < BoardTopology::HexCount; ++hex)
        if (scarce.board.hexes[hex].terrain == Terrain::Mountains &&
            scarce.board.robberHex.value != hex) {
            mountain = HexId{static_cast<std::uint8_t>(hex)};
            break;
        }
    REQUIRE(mountain.has_value());
    const auto vertex = BoardTopology::standard().hexes[mountain->value].vertices.front();
    placeBuilding(scarce, vertex, PlayerId::Two, BuildingType::City);
    setResources(scarce, PlayerId::Two, resources(0, 0, 0, 0, 17));

    MctsSession scarceSearch(scarce, fastConfig(1805));
    const auto scarceResult = scarceSearch.result();
    CHECK(std::none_of(scarceResult.actions.begin(), scarceResult.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlayYearOfPlenty>(evaluation.action);
        }));

    Game reserve(scarce);
    accepted(reserve, PlayYearOfPlenty{});
    accepted(reserve, ChooseYearOfPlentyResource{Resource::Ore});
    accepted(reserve, ChooseYearOfPlentyResource{Resource::Ore});
    accepted(reserve, BeginRoll{});
    accepted(reserve, RollDice{scarce.board.hexes[mountain->value].number});
    CHECK_EQ(reserve.state().players[index(PlayerId::One)].resources[index(Resource::Ore)],
             std::uint8_t{2});
    CHECK_EQ(reserve.state().players[index(PlayerId::Two)].resources[index(Resource::Ore)],
             std::uint8_t{17});
}

TEST(mcts_selects_an_immediate_fifteen_point_win) {
    const auto state = immediateCityWinState();
    CHECK_EQ(victoryPoints(state, PlayerId::One), std::uint8_t{14});
    const auto result = MctsEngine(fastConfig(44)).search(state);
    REQUIRE(result.bestAction.has_value());
    CHECK(std::holds_alternative<BuildCity>(*result.bestAction));
    Game game(state);
    accepted(game, *result.bestAction);
    CHECK_EQ(game.state().winner, std::optional<PlayerId>{PlayerId::One});
    for (const auto& evaluation : result.actions)
        if (std::holds_alternative<BuildCity>(evaluation.action))
            CHECK_NEAR(evaluation.estimatedWinProbability, 1.0, 1e-12);
}

TEST(mcts_chance_root_preserves_simulator_probabilities_and_is_not_selectable) {
    const auto state = blankState(Phase::DiceRoll);
    const Game game(state);
    const auto expected = game.chanceOutcomes();
    auto config = fastConfig(91);
    config.iterations = 2'000;
    config.maxRolloutSteps = 0;
    const auto result = MctsEngine(config).search(state);
    CHECK(result.chanceNode);
    CHECK(!result.bestAction.has_value());
    CHECK_EQ(result.actions.size(), expected.size());
    std::size_t visits = 0;
    double probability = 0.0;
    for (const auto& evaluation : result.actions) {
        REQUIRE(evaluation.chanceProbability.has_value());
        visits += evaluation.visits;
        probability += *evaluation.chanceProbability;
        const auto match = std::find_if(expected.begin(), expected.end(), [&](const auto& outcome) {
            return outcome.action == evaluation.action;
        });
        REQUIRE(match != expected.end());
        CHECK_NEAR(*evaluation.chanceProbability, match->probability, 1e-12);
    }
    CHECK_EQ(visits, config.iterations);
    CHECK_NEAR(probability, 1.0, 1e-12);
}

TEST(mcts_is_reproducible_for_the_same_state_configuration_and_seed) {
    const auto state = blankState();
    const auto config = fastConfig(8675309);
    const auto first = MctsEngine(config).search(state);
    const auto second = MctsEngine(config).search(state);
    CHECK_EQ(first.bestAction, second.bestAction);
    CHECK_EQ(first.actions.size(), second.actions.size());
    for (std::size_t i = 0; i < first.actions.size(); ++i) {
        CHECK_EQ(first.actions[i].action, second.actions[i].action);
        CHECK_EQ(first.actions[i].visits, second.actions[i].visits);
        CHECK_NEAR(first.actions[i].estimatedWinProbability,
                   second.actions[i].estimatedWinProbability, 0.0);
    }
}

TEST(mcts_validates_its_configuration) {
    auto config = fastConfig();
    config.iterations = 0;
    bool rejectedIterations = false;
    try { MctsEngine engine(config); }
    catch (const std::invalid_argument&) { rejectedIterations = true; }
    CHECK(rejectedIterations);

    config = fastConfig();
    config.explorationConstant = -1.0;
    bool rejectedExploration = false;
    try { MctsEngine engine(config); }
    catch (const std::invalid_argument&) { rejectedExploration = true; }
    CHECK(rejectedExploration);

    config = fastConfig();
    config.progressInterval = 0;
    bool rejectedInterval = false;
    try { MctsEngine engine(config); }
    catch (const std::invalid_argument&) { rejectedInterval = true; }
    CHECK(rejectedInterval);
}

TEST(mcts_progress_reuses_one_tree_and_reports_increasing_depth) {
    auto config = fastConfig(2026);
    config.iterations = 80;
    config.progressInterval = 10;
    config.maxRolloutSteps = 4;
    std::vector<MctsResult> updates;
    const auto result = MctsEngine(config).search(
        blankState(), PlayerId::One, std::stop_token{},
        [&](const auto& progress) { updates.push_back(progress); });
    REQUIRE(!updates.empty());
    CHECK_EQ(updates.front().iterations, std::size_t{1});
    CHECK_EQ(updates.back().iterations, config.iterations);
    CHECK_EQ(result.iterations, config.iterations);
    CHECK_EQ(result.maximumDepth, updates.back().maximumDepth);
    for (std::size_t i = 1; i < updates.size(); ++i) {
        CHECK(updates[i].iterations > updates[i - 1].iterations);
        CHECK(updates[i].maximumDepth >= updates[i - 1].maximumDepth);
    }
    CHECK(result.maximumDepth > 0);
}

TEST(mcts_background_search_honours_stop_requests) {
    auto config = fastConfig(77);
    config.iterations = 10'000;
    config.progressInterval = 5;
    config.maxRolloutSteps = 2;
    std::stop_source stop;
    const auto result = MctsEngine(config).search(
        blankState(), PlayerId::One, stop.get_token(),
        [&](const auto& progress) {
            if (progress.iterations >= 20) stop.request_stop();
        });
    CHECK(result.iterations >= 20);
    CHECK(result.iterations < config.iterations);
}

TEST(mcts_session_re_roots_and_retains_the_selected_subtree) {
    auto config = fastConfig(90210);
    config.iterations = 60;
    config.maxRolloutSteps = 4;
    MctsSession session(blankState(), config);

    const auto before = session.run();
    CHECK_EQ(before.iterations, config.iterations);
    REQUIRE(before.bestAction.has_value());
    CHECK(std::holds_alternative<EndTurn>(*before.bestAction));

    CHECK(session.advance(EndTurn{}));
    const auto afterTurn = session.result();
    CHECK_EQ(afterTurn.perspective, PlayerId::Two);
    CHECK_EQ(session.state().turn.phase, Phase::AwaitRoll);
    CHECK_EQ(afterTurn.iterations, config.iterations);
    REQUIRE_EQ(afterTurn.actions.size(), std::size_t{1});
    CHECK(std::holds_alternative<BeginRoll>(afterTurn.actions.front().action));
    CHECK(afterTurn.actions.front().visits > 0);

    const auto retainedBeginRollVisits = afterTurn.actions.front().visits;
    CHECK(session.advance(BeginRoll{}));
    CHECK_EQ(session.state().turn.phase, Phase::DiceRoll);
    CHECK_EQ(session.result().iterations, retainedBeginRollVisits);

    const auto continued = session.run();
    CHECK_EQ(continued.iterations, retainedBeginRollVisits + config.iterations);
}

TEST(explicit_player_perspective_stays_fixed_when_the_opponent_becomes_root) {
    auto config = fastConfig(4242);
    config.iterations = 40;
    config.maxRolloutSteps = 0;
    MctsSession session(blankState(), PlayerId::One, config);
    session.run();
    CHECK(session.advance(EndTurn{}));
    const auto result = session.result();
    CHECK_EQ(result.perspective, PlayerId::One);
    CHECK_EQ(result.rootPlayer, PlayerId::Two);
}

TEST(opponent_root_recommends_the_action_that_minimizes_the_fixed_perspective) {
    auto state = blankState();
    state.activePlayer = PlayerId::Two;
    state.turn.decisionPlayer = PlayerId::Two;
    const auto path = simplePath(2);
    placeBuilding(state, path.vertices[0], PlayerId::Two);
    placeRoad(state, path.edges[0], PlayerId::Two);
    placeRoad(state, path.edges[1], PlayerId::Two);
    setResources(state, PlayerId::Two, SettlementCost);
    CHECK(hasAction<BuildSettlement>(Game(state)));

    auto config = fastConfig(4343);
    config.iterations = 200;
    config.maxRolloutSteps = 0;
    config.explorationConstant = 0.0;
    const auto result = MctsEngine(config).search(state, PlayerId::One);
    REQUIRE(result.bestAction.has_value());
    CHECK(std::holds_alternative<BuildSettlement>(*result.bestAction));
    CHECK_EQ(result.perspective, PlayerId::One);
    CHECK_EQ(result.rootPlayer, PlayerId::Two);
}

TEST(mcts_setup_tree_expects_and_retains_player_twos_back_to_back_placements) {
    Game game(4200112);
    accepted(game, firstAction<PlaceSettlement>(game));
    accepted(game, firstAction<PlaceRoad>(game));
    REQUIRE_EQ(game.state().turn.placementIndex, std::uint8_t{1});
    REQUIRE_EQ(game.state().turn.decisionPlayer, PlayerId::Two);

    auto config = fastConfig(6060);
    config.iterations = 2'000;
    config.maxRolloutSteps = 0;
    MctsSession session(game.state(), config);
    const auto firstPlacement = session.run();
    CHECK_EQ(firstPlacement.perspective, PlayerId::Two);
    CHECK(firstPlacement.maximumDepth >= 3);

    // Follow the most-visited branches without doing any more search. If the
    // original P2-first-placement tree understood the snake order, its retained
    // road child must already contain visited P2-second-placement children.
    const auto mostVisitedAction = [](const MctsResult& result) -> Action {
        const auto selected = std::max_element(result.actions.begin(), result.actions.end(),
            [](const auto& left, const auto& right) { return left.visits < right.visits; });
        REQUIRE(selected != result.actions.end());
        REQUIRE(selected->visits > 0);
        return selected->action;
    };

    CHECK(session.advance(mostVisitedAction(firstPlacement)));
    const auto firstRoad = session.result();
    CHECK_EQ(firstRoad.perspective, PlayerId::Two);
    CHECK_EQ(session.state().turn.phase, Phase::InitialRoad);
    CHECK_EQ(session.state().turn.placementIndex, std::uint8_t{1});

    CHECK(session.advance(mostVisitedAction(firstRoad)));
    const auto secondPlacement = session.result();
    CHECK_EQ(secondPlacement.perspective, PlayerId::Two);
    CHECK_EQ(session.state().turn.phase, Phase::InitialSettlement);
    CHECK_EQ(session.state().turn.placementIndex, std::uint8_t{2});
    CHECK_EQ(session.state().turn.decisionPlayer, PlayerId::Two);
    CHECK(std::all_of(secondPlacement.actions.begin(), secondPlacement.actions.end(),
        [](const auto& evaluation) {
            return std::holds_alternative<PlaceSettlement>(evaluation.action);
        }));
    CHECK(std::any_of(secondPlacement.actions.begin(), secondPlacement.actions.end(),
        [](const auto& evaluation) { return evaluation.visits > 0; }));
}

TEST(root_parallel_results_merge_visits_values_depth_and_recommendation) {
    auto firstConfig = fastConfig(1001);
    auto secondConfig = fastConfig(2002);
    firstConfig.iterations = 100;
    secondConfig.iterations = 100;
    firstConfig.maxRolloutSteps = secondConfig.maxRolloutSteps = 4;
    const auto state = Game(42).state();
    MctsSession first(state, firstConfig);
    MctsSession second(state, secondConfig);
    const auto firstResult = first.run();
    const auto secondResult = second.run();
    const auto merged = mergeMctsResults({firstResult, secondResult});

    CHECK_EQ(merged.workerCount, std::size_t{2});
    CHECK_EQ(merged.iterations, firstResult.iterations + secondResult.iterations);
    CHECK_EQ(merged.maximumDepth, std::max(firstResult.maximumDepth, secondResult.maximumDepth));
    CHECK_EQ(merged.uniqueRootChildren,
             std::min(firstResult.uniqueRootChildren, secondResult.uniqueRootChildren));
    CHECK_EQ(merged.actions.size(), firstResult.actions.size());
    REQUIRE(merged.bestAction.has_value());
    for (std::size_t i = 0; i < merged.actions.size(); ++i) {
        const auto visits = firstResult.actions[i].visits + secondResult.actions[i].visits;
        CHECK_EQ(merged.actions[i].visits, visits);
        if (visits) {
            const auto expected =
                (firstResult.actions[i].estimatedWinProbability * firstResult.actions[i].visits +
                 secondResult.actions[i].estimatedWinProbability * secondResult.actions[i].visits) / visits;
            CHECK_NEAR(merged.actions[i].estimatedWinProbability, expected, 1e-12);
        }
    }

    const auto selected = *merged.bestAction;
    CHECK(first.advance(selected));
    CHECK(second.advance(selected));
    const auto rerooted = mergeMctsResults({first.result(), second.result()});
    const auto selectedEvaluation = std::find_if(merged.actions.begin(), merged.actions.end(),
        [&](const auto& evaluation) { return evaluation.action == selected; });
    REQUIRE(selectedEvaluation != merged.actions.end());
    CHECK_EQ(rerooted.iterations, selectedEvaluation->visits);
}
