#include "catan/Mcts.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>

namespace catan {
namespace {

struct Node;

struct Edge {
    Action action;
    std::optional<double> chanceProbability;
    std::shared_ptr<Node> child;
    bool active{true};
};

struct Node {
    explicit Node(GameState value) : state(std::move(value)) {}

    GameState state;
    std::vector<Edge> edges;
    std::size_t visits{};
    double valueSum{};
    bool initialized{};
    bool chanceNode{};
};

double terminalOrHeuristicValue(const GameState& state, PlayerId perspective) {
    if (state.winner) return *state.winner == perspective ? 1.0 : 0.0;

    auto strength = [&](PlayerId player) {
        const auto& value = state.players[index(player)];
        double score = 4.0 * victoryPoints(state, player);
        score += 0.10 * total(value.resources);
        score += 0.20 * value.playedKnights;
        score += 0.12 * longestRoad(state, player);
        for (std::size_t card = 0; card < DevelopmentCardTypeCount; ++card)
            if (card != index(DevelopmentCard::VictoryPoint)) score += 0.12 * value.developmentCards.cards[card];
        return score;
    };

    const double difference = strength(perspective) - strength(opponent(perspective));
    return 1.0 / (1.0 + std::exp(-difference / 4.0));
}

bool yearOfPlentyCanBeDeferredWithoutChangingProduction(const GameState& state) {
    std::array<std::array<unsigned, ResourceCount>, 13> demand{};
    const auto& topology = BoardTopology::standard();
    for (std::size_t hexIndex = 0; hexIndex < state.board.hexes.size(); ++hexIndex) {
        const auto& hex = state.board.hexes[hexIndex];
        if (hex.number < 2 || hex.number > 12 || hex.number == 7 ||
            state.board.robberHex.value == hexIndex) continue;
        const auto resource = producedResource(hex.terrain);
        if (!resource) continue;
        for (const auto vertex : topology.hexes[hexIndex].vertices) {
            const auto& building = state.board.buildings[vertex.value];
            if (building)
                demand[hex.number][index(*resource)] +=
                    building->type == BuildingType::City ? 2U : 1U;
        }
    }

    // Reserve enough for the largest possible payout and then for both YOP
    // selections. Under this condition taking cards before the roll cannot
    // change production or reserve a card that would be unavailable afterward.
    for (std::size_t resource = 0; resource < ResourceCount; ++resource) {
        unsigned maximumDemand = 0;
        for (std::size_t roll = 2; roll <= 12; ++roll)
            maximumDemand = std::max(maximumDemand, demand[roll][resource]);
        if (state.bank.cards[resource] < maximumDemand + 2) return false;
    }
    return true;
}

bool roadBuildingCanClaimLongestRoad(const GameState& state) {
    const auto player = state.turn.decisionPlayer;
    if (state.awards.longestRoad == player) return false;

    Game afterCard(state);
    if (!afterCard.step(PlayRoadBuilding{}).accepted) return false;
    for (const auto& firstAction : afterCard.legalActions()) {
        if (!std::holds_alternative<PlaceRoad>(firstAction)) continue;
        Game afterFirst(afterCard.state());
        if (!afterFirst.step(firstAction).accepted) continue;
        if (afterFirst.state().awards.longestRoad == player) return true;
        if (afterFirst.state().turn.phase != Phase::FreeRoads) continue;
        for (const auto& secondAction : afterFirst.legalActions()) {
            if (!std::holds_alternative<PlaceRoad>(secondAction)) continue;
            Game afterSecond(afterFirst.state());
            if (afterSecond.step(secondAction).accepted &&
                afterSecond.state().awards.longestRoad == player) return true;
        }
    }
    return false;
}

bool isDominatedSearchAction(const GameState& state, const Action& action) {
    if (std::holds_alternative<Resign>(action)) return true;
    if (const auto* monopoly = std::get_if<PlayMonopoly>(&action)) {
        const auto victim = opponent(state.turn.decisionPlayer);
        return state.players[index(victim)].resources[index(monopoly->resource)] == 0;
    }
    if (state.turn.phase == Phase::AwaitRoll &&
        std::holds_alternative<PlayRoadBuilding>(action)) {
        return !roadBuildingCanClaimLongestRoad(state);
    }
    if (state.turn.phase == Phase::AwaitRoll &&
        std::holds_alternative<PlayYearOfPlenty>(action)) {
        return yearOfPlentyCanBeDeferredWithoutChangingProduction(state);
    }
    return false;
}

void initialize(Node& node) {
    if (node.initialized) return;
    node.initialized = true;
    Game game(node.state);
    const auto chance = game.chanceOutcomes();
    node.chanceNode = !chance.empty();
    if (node.chanceNode) {
        node.edges.reserve(chance.size());
        for (const auto& outcome : chance)
            node.edges.push_back({outcome.action, outcome.probability, nullptr, true});
    } else {
        const auto legal = game.legalActions();
        node.edges.reserve(legal.size());
        for (const auto& action : legal) {
            // Keep dominated operations available to the simulator while
            // excluding them from tree growth. See docs/MCTS_DEVIATIONS.md.
            if (isDominatedSearchAction(node.state, action)) continue;
            node.edges.push_back({action, std::nullopt, nullptr, true});
        }
    }
}

Node& materialize(Node& parent, std::size_t edgeIndex) {
    auto& edge = parent.edges[edgeIndex];
    if (!edge.child) {
        Game next(parent.state);
        const auto result = next.step(edge.action);
        if (!result.accepted)
            throw std::logic_error("MCTS encountered an action rejected by the simulator: " + result.message);
        auto candidate = std::make_shared<Node>(next.state());
        if (!parent.chanceNode) {
            for (std::size_t existingIndex = 0; existingIndex < parent.edges.size(); ++existingIndex) {
                auto& existing = parent.edges[existingIndex];
                if (existingIndex == edgeIndex || !existing.active || !existing.child) continue;
                if (existing.child->state != candidate->state) continue;
                edge.child = existing.child;
                if (edgeIndex < existingIndex) {
                    edge.active = true;
                    existing.active = false;
                } else {
                    edge.active = false;
                }
                return *edge.child;
            }
        }
        edge.child = std::move(candidate);
    }
    return *edge.child;
}

template <typename Random>
std::size_t sampleChanceEdge(const Node& node, Random& random) {
    std::uniform_real_distribution<double> draw(0.0, 1.0);
    double target = draw(random);
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        target -= node.edges[i].chanceProbability.value_or(0.0);
        if (target <= 0.0) return i;
    }
    return node.edges.size() - 1;
}

template <typename Random>
Action rolloutAction(Game& game, Random& random) {
    const auto chance = game.chanceOutcomes();
    if (!chance.empty()) {
        std::uniform_real_distribution<double> draw(0.0, 1.0);
        double target = draw(random);
        for (const auto& outcome : chance) {
            target -= outcome.probability;
            if (target <= 0.0) return outcome.action;
        }
        return chance.back().action;
    }

    const auto legal = game.legalActions();
    if (legal.empty()) throw std::logic_error("non-terminal MCTS rollout has no legal actions");
    std::vector<std::size_t> candidates;
    candidates.reserve(legal.size());
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (!isDominatedSearchAction(game.state(), legal[i])) candidates.push_back(i);
    if (candidates.empty()) return legal.front();
    std::uniform_int_distribution<std::size_t> choose(0, candidates.size() - 1);
    return legal[candidates[choose(random)]];
}

template <typename Random>
double rollout(const GameState& initial, PlayerId perspective, std::size_t maximumSteps, Random& random) {
    Game game(initial);
    for (std::size_t step = 0; step < maximumSteps && !game.state().winner; ++step) {
        const auto result = game.step(rolloutAction(game, random));
        if (!result.accepted) throw std::logic_error("MCTS rollout selected an illegal action");
    }
    return terminalOrHeuristicValue(game.state(), perspective);
}

std::size_t selectDecisionEdge(const Node& node, PlayerId perspective, double explorationConstant) {
    const bool maximizing = node.state.turn.decisionPlayer == perspective;
    std::size_t bestIndex = 0;
    double bestScore = -std::numeric_limits<double>::infinity();
    const double parentLog = std::log(static_cast<double>(std::max<std::size_t>(1, node.visits)));
    for (std::size_t i = 0; i < node.edges.size(); ++i) {
        if (!node.edges[i].active) continue;
        const auto& child = *node.edges[i].child;
        const double mean = child.valueSum / child.visits;
        const double exploitation = maximizing ? mean : 1.0 - mean;
        const double exploration = explorationConstant * std::sqrt(parentLog / child.visits);
        const double score = exploitation + exploration;
        if (score > bestScore) {
            bestScore = score;
            bestIndex = i;
        }
    }
    return bestIndex;
}

template <typename Random>
std::size_t runIteration(Node& root, PlayerId perspective, const MctsConfig& config, Random& random) {
    Node* node = &root;
    std::vector<Node*> path{node};

    while (!node->state.winner) {
        initialize(*node);
        if (node->edges.empty()) break;

        std::size_t edgeIndex = 0;
        bool expanded = false;
        if (node->chanceNode) {
            edgeIndex = sampleChanceEdge(*node, random);
            expanded = !node->edges[edgeIndex].child;
        } else {
            std::vector<std::size_t> unexpanded;
            for (std::size_t i = 0; i < node->edges.size(); ++i)
                if (!node->edges[i].child) unexpanded.push_back(i);
            if (!unexpanded.empty()) {
                std::uniform_int_distribution<std::size_t> choose(0, unexpanded.size() - 1);
                edgeIndex = unexpanded[choose(random)];
                expanded = true;
            } else {
                edgeIndex = selectDecisionEdge(*node, perspective, config.explorationConstant);
            }
        }

        node = &materialize(*node, edgeIndex);
        path.push_back(node);
        if (expanded) break;
    }

    const double value = node->state.winner
        ? terminalOrHeuristicValue(node->state, perspective)
        : rollout(node->state, perspective, config.maxRolloutSteps, random);
    for (Node* visited : path) {
        ++visited->visits;
        visited->valueSum += value;
    }
    return path.size() - 1;
}

MctsResult makeResult(Node& root, PlayerId perspective, std::size_t iterations,
                      std::size_t maximumDepth) {
    initialize(root);
    MctsResult result;
    result.perspective = perspective;
    result.rootPlayer = root.state.turn.decisionPlayer;
    result.iterations = iterations;
    result.maximumDepth = maximumDepth;
    result.chanceNode = root.chanceNode;
    result.uniqueRootChildren = static_cast<std::size_t>(std::count_if(
        root.edges.begin(), root.edges.end(), [](const auto& edge) { return edge.active; }));
    result.actions.reserve(root.edges.size());
    for (const auto& edge : root.edges) {
        const std::size_t visits = edge.child ? edge.child->visits : 0;
        const double probability = visits ? edge.child->valueSum / visits : 0.5;
        result.actions.push_back({edge.action, visits, probability, edge.chanceProbability});
    }

    if (!result.chanceNode && !result.actions.empty()) {
        const bool maximizing = result.rootPlayer == perspective;
        const ActionEvaluation* best = nullptr;
        for (const auto& evaluation : result.actions) {
            if (evaluation.visits == 0) continue;
            const bool better = best && (maximizing
                ? evaluation.estimatedWinProbability > best->estimatedWinProbability
                : evaluation.estimatedWinProbability < best->estimatedWinProbability);
            if (!best || better ||
                (evaluation.estimatedWinProbability == best->estimatedWinProbability &&
                 evaluation.visits > best->visits)) best = &evaluation;
        }
        if (best) result.bestAction = best->action;
    }
    return result;
}

void validateConfig(const MctsConfig& config) {
    if (config.iterations == 0) throw std::invalid_argument("MCTS iterations must be greater than zero");
    if (config.progressInterval == 0) throw std::invalid_argument("MCTS progress interval must be greater than zero");
    if (!std::isfinite(config.explorationConstant) || config.explorationConstant < 0.0)
        throw std::invalid_argument("MCTS exploration constant must be finite and non-negative");
}

std::size_t retainedTreeDepth(const Node& node) {
    std::size_t depth = 0;
    for (const auto& edge : node.edges)
        if (edge.active && edge.child)
            depth = std::max(depth, std::size_t{1} + retainedTreeDepth(*edge.child));
    return depth;
}

void invertPerspective(Node& node) {
    node.valueSum = static_cast<double>(node.visits) - node.valueSum;
    for (auto& edge : node.edges)
        if (edge.active && edge.child) invertPerspective(*edge.child);
}

} // namespace

struct MctsSession::Impl {
    Impl(GameState state, PlayerId searchPerspective, MctsConfig searchConfig,
         bool followDecisionPerspectiveValue)
        : root(std::make_shared<Node>(std::move(state))), perspective(searchPerspective),
          config(searchConfig), random(searchConfig.seed),
          followDecisionPerspective(followDecisionPerspectiveValue) {
        initialize(*root);
    }

    std::shared_ptr<Node> root;
    PlayerId perspective;
    MctsConfig config;
    std::mt19937 random;
    std::size_t maximumDepth{};
    bool followDecisionPerspective{};
};

MctsSession::MctsSession(GameState state, MctsConfig config)
{
    validateConfig(config);
    const auto perspective = state.turn.decisionPlayer;
    impl_ = std::make_unique<Impl>(std::move(state), perspective, config, true);
}

MctsSession::MctsSession(GameState state, PlayerId perspective, MctsConfig config) {
    validateConfig(config);
    impl_ = std::make_unique<Impl>(std::move(state), perspective, config, false);
}

MctsSession::~MctsSession() = default;
MctsSession::MctsSession(MctsSession&&) noexcept = default;
MctsSession& MctsSession::operator=(MctsSession&&) noexcept = default;

MctsResult MctsSession::run() {
    return run(std::stop_token{}, {});
}

MctsResult MctsSession::run(std::stop_token stopToken, const MctsProgressCallback& progress) {
    std::size_t added = 0;
    std::size_t lastPublished = 0;
    while (added < impl_->config.iterations && !stopToken.stop_requested()) {
        impl_->maximumDepth = std::max(
            impl_->maximumDepth,
            runIteration(*impl_->root, impl_->perspective, impl_->config, impl_->random));
        ++added;
        if (progress && (added == 1 || added % impl_->config.progressInterval == 0)) {
            progress(result());
            lastPublished = added;
        }
    }
    auto current = result();
    if (progress && added != lastPublished) progress(current);
    return current;
}

MctsResult MctsSession::result() {
    return makeResult(*impl_->root, impl_->perspective, impl_->root->visits, impl_->maximumDepth);
}

bool MctsSession::advance(const Action& action) {
    initialize(*impl_->root);
    const auto edge = std::find_if(impl_->root->edges.begin(), impl_->root->edges.end(),
        [&](const auto& candidate) { return candidate.action == action; });
    if (edge == impl_->root->edges.end()) {
        // A simulator-legal action can be intentionally absent from the search
        // tree (for example, a zero-yield Monopoly). Apply it and start a fresh
        // tree rather than making the interactive stepper depend on pruning.
        Game next(impl_->root->state);
        const auto step = next.step(action);
        if (!step.accepted)
            throw std::invalid_argument("cannot advance MCTS tree with an unavailable action");
        impl_->root = std::make_shared<Node>(next.state());
        if (impl_->followDecisionPerspective)
            impl_->perspective = impl_->root->state.turn.decisionPlayer;
        impl_->maximumDepth = 0;
        initialize(*impl_->root);
        return false;
    }

    const bool reused = edge->child && edge->child->visits > 0;
    const auto edgeIndex = static_cast<std::size_t>(edge - impl_->root->edges.begin());
    materialize(*impl_->root, edgeIndex);
    auto nextRoot = impl_->root->edges[edgeIndex].child;
    impl_->root = std::move(nextRoot);

    const PlayerId nextPerspective = impl_->root->state.turn.decisionPlayer;
    if (impl_->followDecisionPerspective && nextPerspective != impl_->perspective) {
        invertPerspective(*impl_->root);
        impl_->perspective = nextPerspective;
    }
    impl_->maximumDepth = retainedTreeDepth(*impl_->root);
    return reused;
}

const GameState& MctsSession::state() const {
    return impl_->root->state;
}

MctsEngine::MctsEngine(MctsConfig config) : config_(config) {
    validateConfig(config_);
}

MctsResult mergeMctsResults(const std::vector<MctsResult>& results) {
    if (results.empty()) throw std::invalid_argument("cannot merge an empty set of MCTS results");
    MctsResult merged = results.front();
    merged.iterations = 0;
    merged.maximumDepth = 0;
    merged.workerCount = 0;
    merged.uniqueRootChildren = merged.actions.size();
    merged.bestAction.reset();
    std::vector<double> valueSums(merged.actions.size(), 0.0);
    for (auto& action : merged.actions) {
        action.visits = 0;
        action.estimatedWinProbability = 0.5;
    }

    for (const auto& result : results) {
        if (result.perspective != merged.perspective || result.rootPlayer != merged.rootPlayer ||
            result.chanceNode != merged.chanceNode ||
            result.actions.size() != merged.actions.size())
            throw std::invalid_argument("incompatible MCTS results cannot be merged");
        merged.iterations += result.iterations;
        merged.maximumDepth = std::max(merged.maximumDepth, result.maximumDepth);
        merged.workerCount += result.workerCount;
        merged.uniqueRootChildren = std::min(merged.uniqueRootChildren, result.uniqueRootChildren);
        for (std::size_t i = 0; i < result.actions.size(); ++i) {
            if (result.actions[i].action != merged.actions[i].action)
                throw std::invalid_argument("MCTS result action order differs between workers");
            merged.actions[i].visits += result.actions[i].visits;
            valueSums[i] += result.actions[i].estimatedWinProbability * result.actions[i].visits;
        }
    }
    for (std::size_t i = 0; i < merged.actions.size(); ++i)
        if (merged.actions[i].visits)
            merged.actions[i].estimatedWinProbability = valueSums[i] / merged.actions[i].visits;

    if (!merged.chanceNode) {
        const bool maximizing = merged.rootPlayer == merged.perspective;
        const ActionEvaluation* best = nullptr;
        for (const auto& evaluation : merged.actions) {
            if (!evaluation.visits) continue;
            const bool better = best && (maximizing
                ? evaluation.estimatedWinProbability > best->estimatedWinProbability
                : evaluation.estimatedWinProbability < best->estimatedWinProbability);
            if (!best || better ||
                (evaluation.estimatedWinProbability == best->estimatedWinProbability &&
                 evaluation.visits > best->visits)) best = &evaluation;
        }
        if (best) merged.bestAction = best->action;
    }
    return merged;
}

MctsResult MctsEngine::search(const GameState& state) const {
    return search(state, state.turn.decisionPlayer);
}

MctsResult MctsEngine::search(const GameState& state, PlayerId perspective) const {
    return search(state, perspective, std::stop_token{}, {});
}

MctsResult MctsEngine::search(const GameState& state, PlayerId perspective,
                              std::stop_token stopToken, const MctsProgressCallback& progress) const {
    MctsSession session(state, perspective, config_);
    return session.run(stopToken, progress);
}

} // namespace catan
