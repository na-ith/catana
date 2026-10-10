#include "catan/Mcts.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace catan;

struct Scenario {
    std::string name;
    GameState state;
    std::optional<InformationState> information;
};

template <typename ActionType>
ActionType firstAction(const Game& game) {
    for (const auto& action : game.legalTransformations())
        if (const auto* selected = std::get_if<ActionType>(&action)) return *selected;
    throw std::runtime_error("scenario setup could not find a required legal action");
}

void apply(Game& game, const Action& action) {
    const auto result = game.transform(action);
    if (!result.accepted) throw std::runtime_error("scenario setup action rejected: " + result.message);
}

Game afterPlacements(std::uint32_t seed) {
    Game game(seed);
    for (unsigned placement = 0; placement < 4; ++placement) {
        apply(game, firstAction<PlaceSettlement>(game));
        apply(game, firstAction<PlaceRoad>(game));
    }
    return game;
}

Game earlyMainGame(std::uint32_t seed) {
    Game game = afterPlacements(seed);
    apply(game, BeginRoll{});
    const auto outcomes = game.chanceOutcomes();
    const auto six = std::find_if(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
        const auto* roll = std::get_if<RollDice>(&outcome.action);
        return roll && roll->sum == 6;
    });
    if (six == outcomes.end()) throw std::runtime_error("roll six was unavailable in setup scenario");
    apply(game, six->action);
    if (game.state().turn.phase != Phase::Main)
        throw std::runtime_error("roll six did not produce the expected main phase");
    return game;
}

GameState midgameState(std::uint32_t seed) {
    Game game = afterPlacements(seed);
    // Play twelve complete turns without building. Sampling ordinary non-seven
    // rolls creates realistic accumulated hands while keeping fixture creation
    // deterministic and outside the timed search.
    for (unsigned turn = 0; turn < 12 && !game.state().winner; ++turn) {
        apply(game, BeginRoll{});
        const auto outcomes = game.chanceOutcomes();
        const auto ordinaryRoll = std::find_if(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
            const auto* roll = std::get_if<RollDice>(&outcome.action);
            return roll && roll->sum != 7;
        });
        if (ordinaryRoll == outcomes.end()) throw std::runtime_error("no ordinary dice outcome in midgame setup");
        apply(game, ordinaryRoll->action);
        if (game.state().turn.phase != Phase::Main)
            throw std::runtime_error("ordinary roll did not produce the expected main phase");
        apply(game, EndTurn{});
    }

    apply(game, BeginRoll{});
    const auto outcomes = game.chanceOutcomes();
    const auto ordinaryRoll = std::find_if(outcomes.begin(), outcomes.end(), [](const auto& outcome) {
        const auto* roll = std::get_if<RollDice>(&outcome.action);
        return roll && roll->sum != 7;
    });
    if (ordinaryRoll == outcomes.end()) throw std::runtime_error("no ordinary dice outcome for midgame root");
    apply(game, ordinaryRoll->action);
    if (game.state().turn.phase != Phase::Main)
        throw std::runtime_error("midgame root is not a main decision");
    return game.state();
}

void replaceResources(GameState& state, PlayerId player, const Resources& replacement) {
    auto& hand = state.players[index(player)].resources;
    for (std::size_t resource = 0; resource < ResourceCount; ++resource) {
        state.bank.cards[resource] = static_cast<std::uint8_t>(state.bank.cards[resource] + hand[resource]);
        hand[resource] = 0;
    }
    for (std::size_t resource = 0; resource < ResourceCount; ++resource) {
        if (replacement[resource] > state.bank.cards[resource])
            throw std::runtime_error("scenario requested more resources than the bank contains");
        state.bank.cards[resource] = static_cast<std::uint8_t>(state.bank.cards[resource] - replacement[resource]);
        hand[resource] = replacement[resource];
    }
}

GameState asDecision(const GameState& source, Phase phase) {
    auto state = source;
    state.activePlayer = PlayerId::One;
    state.turn.phase = phase;
    state.turn.returnPhase = Phase::Main;
    state.turn.decisionPlayer = PlayerId::One;
    state.turn.pendingDiscards = {};
    state.turn.pendingInitialSettlement.reset();
    state.turn.placementIndex = 0;
    state.turn.freeRoadsRemaining = 0;
    state.turn.plentyPicksRemaining = 0;
    state.turn.playedDevelopmentCard = false;
    return state;
}

std::vector<Scenario> scenarios(std::uint32_t seed) {
    Game initial(seed);
    Game initialRoad(seed);
    apply(initialRoad, firstAction<PlaceSettlement>(initialRoad));

    Game chance = afterPlacements(seed);
    apply(chance, BeginRoll{});

    const auto main = midgameState(seed);
    auto rich = asDecision(main, Phase::Main);
    replaceResources(rich, PlayerId::One, Resources{5, 5, 5, 5, 5});
    replaceResources(rich, PlayerId::Two, Resources{5, 5, 5, 5, 5});

    auto robber = asDecision(rich, Phase::MoveRobber);
    robber.turn.returnPhase = Phase::Main;
    // Ensure opponent-targeting robber choices are represented in this fixture.
    robber.awards.largestArmy = PlayerId::Two;

    auto discard = asDecision(rich, Phase::Discarding);
    discard.turn.pendingDiscards = {3, 3};
    discard.turn.returnPhase = Phase::MoveRobber;

    auto opponentTurn = asDecision(rich, Phase::Main);
    opponentTurn.activePlayer = PlayerId::Two;
    opponentTurn.turn.decisionPlayer = PlayerId::Two;
    InformationState hiddenCard(opponentTurn, PlayerId::One);
    if (!hiddenCard.transform(BuyDevelopmentCard{}).accepted ||
        !hiddenCard.transform(DrawUnknownDevelopmentCard{}).accepted)
        throw std::runtime_error("could not prepare hidden development-card information scenario");

    return {
        {"initial_settlement", initial.state(), std::nullopt},
        {"initial_road", initialRoad.state(), std::nullopt},
        {"dice_chance", chance.state(), std::nullopt},
        {"early_main", earlyMainGame(seed).state(), std::nullopt},
        {"midgame_main", main, std::nullopt},
        {"trade_rich_main", rich, std::nullopt},
        {"robber_decision", robber, std::nullopt},
        {"simultaneous_discard", discard, std::nullopt},
        {"hidden_card_information", {}, std::move(hiddenCard)},
    };
}

struct Options {
    std::string scenario{"all"};
    std::size_t iterations{2000};
    std::size_t repeats{3};
    std::size_t rolloutSteps{500};
    std::size_t maxTradeBatchStates{512};
    std::uint32_t seed{42};
};

Options parseOptions(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--help") {
            std::cout << "catan_mcts_benchmark [--scenario NAME|all] [--iterations N] [--repeats N] "
                         "[--rollout-steps N] [--trade-batch-states N] [--seed N]\n";
            std::exit(0);
        }
        if (i + 1 >= argc) throw std::invalid_argument("missing value for " + std::string(name));
        const std::string_view value(argv[++i]);
        if (name == "--scenario") options.scenario = value;
        else if (name == "--iterations") options.iterations = std::stoull(std::string(value));
        else if (name == "--repeats") options.repeats = std::stoull(std::string(value));
        else if (name == "--rollout-steps") options.rolloutSteps = std::stoull(std::string(value));
        else if (name == "--trade-batch-states") options.maxTradeBatchStates = std::stoull(std::string(value));
        else if (name == "--seed") options.seed = static_cast<std::uint32_t>(std::stoul(std::string(value)));
        else throw std::invalid_argument("unknown option: " + std::string(name));
    }
    if (options.iterations == 0 || options.repeats == 0)
        throw std::invalid_argument("iterations and repeats must be positive");
    return options;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parseOptions(argc, argv);
        auto allScenarios = scenarios(options.seed);
        if (options.scenario != "all") {
            std::erase_if(allScenarios, [&](const auto& item) { return item.name != options.scenario; });
            if (allScenarios.empty()) throw std::invalid_argument("unknown scenario: " + options.scenario);
        }

        std::cout << "scenario,repeat,iterations,elapsed_seconds,iterations_per_second,root_actions,max_depth,worlds\n";
        for (const auto& scenario : allScenarios) {
            for (std::size_t repeat = 0; repeat < options.repeats; ++repeat) {
                MctsConfig config;
                config.iterations = options.iterations;
                config.maxRolloutSteps = options.rolloutSteps;
                config.maxTradeBatchStates = options.maxTradeBatchStates;
                config.seed = options.seed + static_cast<std::uint32_t>(repeat);

                const auto started = std::chrono::steady_clock::now();
                const MctsEngine engine(config);
                const auto result = scenario.information
                    ? engine.search(*scenario.information) : engine.search(scenario.state);
                const auto elapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - started).count();
                const double rate = elapsed > 0.0 ? result.iterations / elapsed : 0.0;
                std::cout << scenario.name << ',' << repeat + 1 << ',' << result.iterations << ','
                          << std::fixed << std::setprecision(6) << elapsed << ',' << rate << ','
                          << result.actions.size() << ',' << result.maximumDepth << ','
                          << (scenario.information ? scenario.information->worldCount() : 1) << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "MCTS benchmark failed: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
