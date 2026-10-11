#include "catan/Mcts.h"
#include "catan/TradePolicy.h"

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
    explicit Node(GameState value, PlayerId view, std::size_t batchLimit)
        : state(std::move(value)), perspective(view), maxTradeBatchStates(batchLimit) {}

    GameState state;
    PlayerId perspective;
    std::size_t maxTradeBatchStates;
    // Internal policy node, not an intermediate simulator state.
    std::optional<ResolveDiscards> pendingDiscard;
    PlayerId discardActor{};
    std::optional<search::TradeContext> trading;
    std::vector<Edge> edges;
    std::size_t visits{};
    double valueSum{};
    bool initialized{};
    bool chanceNode{};
    PlayerId actor() const {
        return state.turn.phase == Phase::Discarding && state.turn.pendingDiscards[index(perspective)]
            ? perspective : state.turn.decisionPlayer;
    }
};

double terminalOrHeuristicValue(const GameState& state, PlayerId perspective) {
    if (state.winner) return *state.winner == perspective ? 1.0 : 0.0;

    auto strength = [&](PlayerId player) {
        return 1.0 / (15.0 - victoryPoints(state, player));
    };

    const double difference = strength(perspective) - strength(opponent(perspective));
    return 1.0 / (1.0 + std::exp(-difference));
}

bool isPrunedSearchAction(const GameState& state, const Action& action) {
    if (std::holds_alternative<Resign>(action)) return true;
    if (state.turn.phase == Phase::InitialSettlement) {
        if (const auto* settlement = std::get_if<PlaceSettlement>(&action)) {
            unsigned productiveHexes = 0;
            for (const auto hex : BoardTopology::standard().vertices[settlement->vertex.value].hexes)
                if (hex && producedResource(state.board.hexes[hex->value].terrain)) ++productiveHexes;
            return productiveHexes <= 1;
        }
    }
    if (const auto* robber = std::get_if<MoveRobber>(&action)) {
        const auto player = state.turn.decisionPlayer;
        const auto victim = opponent(player);
        bool touchesSelf = false;
        bool touchesOpponent = false;
        for (const auto vertex : BoardTopology::standard().hexes[robber->hex.value].vertices) {
            const auto& building = state.board.buildings[vertex.value];
            if (!building) continue;
            touchesSelf |= building->owner == player;
            touchesOpponent |= building->owner == victim;
        }
        if (touchesSelf && !touchesOpponent) return true;
        // Match the simulator's friendly-robber threshold; an empty opponent
        // hand still makes blocking their production worthwhile.
        return publicVictoryPoints(state, victim) > 2 && !touchesOpponent;
    }
    if (const auto* monopoly = std::get_if<PlayMonopoly>(&action)) {
        const auto victim = opponent(state.turn.decisionPlayer);
        return state.players[index(victim)].resources[index(monopoly->resource)] == 0;
    }
    // Defer both cards until after the roll. Year of Plenty's rare bank-shortage
    // exception is deliberately ignored for search performance (see MCTS_DEVIATIONS.md).
    if (state.turn.phase == Phase::AwaitRoll &&
        (std::holds_alternative<PlayRoadBuilding>(action) ||
         std::holds_alternative<PlayYearOfPlenty>(action))) return true;
    return false;
}

void initialize(Node& node) {
    if (node.initialized) return;
    node.initialized = true;
    Game game(node.state);
    if (node.pendingDiscard) {
        node.chanceNode = true; // Opponent-policy randomness, not game chance.
        const auto choices = game.discardChoices(opponent(node.discardActor));
        for (const auto& choice : choices) {
            auto joint = *node.pendingDiscard;
            joint.discarded[index(opponent(node.discardActor))] = std::get<DiscardResources>(choice).cards;
            node.edges.push_back({joint, 1.0 / choices.size(), nullptr, true});
        }
        return;
    }
    const auto chance = game.chanceOutcomes();
    node.chanceNode = !chance.empty();
    if (node.chanceNode) {
        node.edges.reserve(chance.size());
        for (const auto& outcome : chance)
            node.edges.push_back({outcome.action, outcome.probability, nullptr, true});
    } else {
        auto legal = node.state.turn.phase == Phase::Discarding
            ? game.discardChoices(node.actor()) : game.legalTransformations();
        if (std::any_of(legal.begin(), legal.end(), [](const auto& action) {
                return std::holds_alternative<MaritimeTrade>(action);
            })) {
            if (node.trading && node.trading->lastWasBatch) {
                // The preceding exhaustive batch already included all longer
                // prefixes. Do not enumerate bundles we will discard anyway.
                std::erase_if(legal, [](const auto& action) { return std::holds_alternative<MaritimeTrade>(action); });
            } else {
                auto trades = search::tradeOptions(node.state, node.maxTradeBatchStates);
                if (trades.complete) {
                    std::erase_if(legal, [](const auto& action) { return std::holds_alternative<MaritimeTrade>(action); });
                    legal.insert(legal.end(), trades.batches.begin(), trades.batches.end());
                }
            }
        }
        const bool hasPreferredAction = std::any_of(legal.begin(), legal.end(),
            [&](const Action& action) { return !isPrunedSearchAction(node.state, action); });
        node.edges.reserve(legal.size());
        for (const auto& action : legal) {
            // Restore legal non-resigning choices if heuristics remove every
            // continuation. See docs/MCTS_DEVIATIONS.md.
            if (std::holds_alternative<Resign>(action) ||
                (hasPreferredAction && isPrunedSearchAction(node.state, action)) ||
                search::redundantTrade(node.state, node.trading ? &*node.trading : nullptr, action)) continue;
            node.edges.push_back({action, std::nullopt, nullptr, true});
        }
    }
}

Node& materialize(Node& parent, std::size_t edgeIndex) {
    auto& edge = parent.edges[edgeIndex];
    if (!edge.child) {
        if (const auto* choice = std::get_if<DiscardResources>(&edge.action)) {
            ResolveDiscards joint{};
            joint.discarded[index(parent.actor())] = choice->cards;
            if (parent.state.turn.pendingDiscards[index(opponent(parent.actor()))]) {
                auto policy = std::make_shared<Node>(parent.state, parent.perspective, parent.maxTradeBatchStates);
                policy->pendingDiscard = joint;
                policy->discardActor = parent.actor();
                edge.child = std::move(policy);
                return *edge.child;
            }
            Game next(parent.state);
            if (!next.transform(joint).accepted) throw std::logic_error("invalid discard choice");
            edge.child = std::make_shared<Node>(next.state(), parent.perspective, parent.maxTradeBatchStates);
            return *edge.child;
        }
        Game next(parent.state);
        const auto result = next.transform(edge.action);
        if (!result.accepted)
            throw std::logic_error("MCTS encountered an action rejected by the simulator: " + result.message);
        auto candidate = std::make_shared<Node>(next.state(), parent.perspective, parent.maxTradeBatchStates);
        candidate->trading = search::nextTradeContext(parent.state, parent.trading ? &*parent.trading : nullptr, edge.action);
        if (!parent.chanceNode) {
            for (std::size_t existingIndex = 0; existingIndex < parent.edges.size(); ++existingIndex) {
                auto& existing = parent.edges[existingIndex];
                if (existingIndex == edgeIndex || !existing.active || !existing.child) continue;
                if (existing.child->pendingDiscard || existing.child->state != candidate->state ||
                    existing.child->trading != candidate->trading) continue;
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
Action rolloutAction(Game& game, Random& random, const search::TradeContext* trading) {
    if (game.state().turn.phase == Phase::Discarding) {
        ResolveDiscards joint{};
        for (std::size_t p = 0; p < PlayerCount; ++p) {
            const auto choices = game.discardChoices(static_cast<PlayerId>(p));
            if (choices.empty()) continue;
            std::uniform_int_distribution<std::size_t> choose(0, choices.size() - 1);
            joint.discarded[p] = std::get<DiscardResources>(choices[choose(random)]).cards;
        }
        return joint;
    }
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

    const auto legal = game.legalTransformations();
    if (legal.empty()) throw std::logic_error("non-terminal MCTS rollout has no legal actions");
    std::vector<std::size_t> candidates;
    candidates.reserve(legal.size());
    for (std::size_t i = 0; i < legal.size(); ++i)
        if (!isPrunedSearchAction(game.state(), legal[i]) &&
            !(trading && trading->lastWasBatch && std::holds_alternative<MaritimeTrade>(legal[i])) &&
            !search::redundantTrade(game.state(), trading, legal[i])) candidates.push_back(i);
    if (candidates.empty()) {
        for (std::size_t i = 0; i < legal.size(); ++i)
            if (!std::holds_alternative<Resign>(legal[i])) candidates.push_back(i);
    }
    if (candidates.empty()) return legal.front();
    std::uniform_int_distribution<std::size_t> choose(0, candidates.size() - 1);
    const auto selected = legal[candidates[choose(random)]];
    if (const auto* trade = std::get_if<MaritimeTrade>(&selected)) {
        MaritimeTradeBatch batch{{*trade}};
        Game working(game.state());
        working.transform(*trade);
        std::bernoulli_distribution continueTrading(0.5);
        while (continueTrading(random)) {
            std::vector<MaritimeTrade> options;
            for (const auto& action : working.legalTransformations())
                if (const auto* next = std::get_if<MaritimeTrade>(&action)) options.push_back(*next);
            if (options.empty()) break;
            std::uniform_int_distribution<std::size_t> pick(0, options.size() - 1);
            const auto next = options[pick(random)];
            working.transform(next);
            batch.trades.push_back(next);
        }
        return batch;
    }
    return selected;
}

template <typename Random>
double rollout(const GameState& initial, PlayerId perspective, std::size_t maximumSteps, Random& random,
               std::optional<search::TradeContext> trading) {
    Game game(initial);
    for (std::size_t step = 0; step < maximumSteps && !game.state().winner; ++step) {
        const auto action = rolloutAction(game, random, trading ? &*trading : nullptr);
        trading = search::nextTradeContext(game.state(), trading ? &*trading : nullptr, action);
        const auto result = game.transform(action);
        if (!result.accepted) throw std::logic_error("MCTS rollout selected an illegal action");
    }
    return terminalOrHeuristicValue(game.state(), perspective);
}

std::size_t selectDecisionEdge(const Node& node, PlayerId perspective, double explorationConstant) {
    const bool maximizing = node.actor() == perspective;
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
        // A freshly expanded choice must resolve its fixed discard before
        // rollout; otherwise rollout would silently replace that choice.
        if (expanded && !node->pendingDiscard) break;
    }

    const double value = node->state.winner
        ? terminalOrHeuristicValue(node->state, perspective)
        : rollout(node->state, perspective, config.maxRolloutSteps, random, node->trading);
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
    result.rootPlayer = root.actor();
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
    node.perspective = opponent(node.perspective);
    node.valueSum = static_cast<double>(node.visits) - node.valueSum;
    if (node.state.turn.phase == Phase::Discarding) {
        // Follow-decision sessions can change perspective. An already-built
        // simultaneous node chooses vectors for the old perspective, so its
        // conditional topology cannot be relabelled for the other player.
        node.edges.clear();
        node.initialized = false;
        node.chanceNode = false;
        node.pendingDiscard.reset();
        return;
    }
    for (auto& edge : node.edges)
        if (edge.active && edge.child) invertPerspective(*edge.child);
}

} // namespace

struct MctsSession::Impl {
    Impl(GameState state, PlayerId searchPerspective, MctsConfig searchConfig,
         bool followDecisionPerspectiveValue)
        : root(std::make_shared<Node>(std::move(state), searchPerspective, searchConfig.maxTradeBatchStates)), perspective(searchPerspective),
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
    if (impl_->root->state.turn.phase == Phase::Discarding &&
        !Game(impl_->root->state).transform(action).accepted)
        throw std::invalid_argument("cannot advance with incomplete or invalid discards");
    initialize(*impl_->root);
    if (const auto* joint = std::get_if<ResolveDiscards>(&action)) {
        Game next(impl_->root->state);
        if (!next.transform(action).accepted)
            throw std::invalid_argument("invalid simultaneous discards");
        for (std::size_t i = 0; i < impl_->root->edges.size(); ++i) {
            const auto* choice = std::get_if<DiscardResources>(&impl_->root->edges[i].action);
            if (!choice || choice->cards != joint->discarded[index(impl_->root->actor())]) continue;
            materialize(*impl_->root, i);
            auto child = impl_->root->edges[i].child;
            if (child->pendingDiscard) {
                initialize(*child);
                for (std::size_t j = 0; j < child->edges.size(); ++j)
                    if (child->edges[j].action == action) {
                        materialize(*child, j);
                        child = child->edges[j].child;
                        break;
                    }
            }
            if (child->state != next.state() || child->pendingDiscard) break;
            const bool reused = child->visits > 0;
            impl_->root = std::move(child);
            impl_->maximumDepth = retainedTreeDepth(*impl_->root);
            if (impl_->followDecisionPerspective && impl_->perspective != impl_->root->actor()) {
                invertPerspective(*impl_->root);
                impl_->perspective = impl_->root->actor();
            }
            return reused;
        }
    }
    auto edge = std::find_if(impl_->root->edges.begin(), impl_->root->edges.end(),
        [&](const auto& candidate) { return candidate.action == action; });
    if (edge == impl_->root->edges.end() && std::holds_alternative<MaritimeTradeBatch>(action)) {
        Game observed(impl_->root->state);
        if (!observed.transform(action).accepted)
            throw std::invalid_argument("invalid trade batch");
        // External callers may supply another valid ordering of the same
        // trades. Reuse the canonical result rather than require its spelling.
        edge = std::find_if(impl_->root->edges.begin(), impl_->root->edges.end(), [&](const auto& candidate) {
            if (!std::holds_alternative<MaritimeTradeBatch>(candidate.action)) return false;
            Game canonical(impl_->root->state);
            return canonical.transform(candidate.action).accepted && canonical.state() == observed.state();
        });
    }
    if (edge == impl_->root->edges.end()) {
        // A simulator-legal action can be intentionally absent from the search
        // tree (for example, a zero-yield Monopoly). Apply it and start a fresh
        // tree rather than making the interactive stepper depend on pruning.
        Game next(impl_->root->state);
        const auto step = next.transform(action);
        if (!step.accepted)
            throw std::invalid_argument("cannot advance MCTS tree with an unavailable action");
        impl_->root = std::make_shared<Node>(next.state(), impl_->perspective, impl_->config.maxTradeBatchStates);
        if (impl_->followDecisionPerspective)
            impl_->perspective = impl_->root->state.turn.decisionPlayer;
        impl_->root->perspective = impl_->perspective;
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
