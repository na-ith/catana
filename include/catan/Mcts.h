#pragma once

#include "catan/Game.h"
#include "catan/InformationState.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

namespace catan {

struct MctsConfig {
    std::size_t iterations{2'000};
    std::size_t maxRolloutSteps{500};
    double explorationConstant{1.4142135623730951};
    std::uint32_t seed{1};
    std::size_t progressInterval{100};
    // Exhaustive distinct trade results per node, including the initial state.
    // If exceeded, retain ordinary single trades rather than omit any bundles.
    std::size_t maxTradeBatchStates{512};
};

struct ActionEvaluation {
    Action action;
    std::size_t visits{};
    double estimatedWinProbability{};
    std::optional<double> chanceProbability{};
};

struct MctsResult {
    PlayerId perspective{PlayerId::One};
    PlayerId rootPlayer{PlayerId::One};
    std::size_t iterations{};
    std::size_t maximumDepth{};
    std::size_t workerCount{1};
    std::size_t uniqueRootChildren{};
    bool chanceNode{};
    std::vector<ActionEvaluation> actions;
    std::optional<Action> bestAction{};
};

// Combines independent root-parallel searches. Visit-weighted action values
// are merged without attempting to combine the internal trees.
MctsResult mergeMctsResults(const std::vector<MctsResult>& results);

using MctsProgressCallback = std::function<void(const MctsResult&)>;

// Stateful search tree that can be advanced after a real action. Advancing
// retains the selected child's statistics and descendants and deletes only
// unreachable sibling branches. Different decision actions that produce an
// identical complete GameState share one sibling child and its statistics.
// Search-path trade-order metadata must also match before folding children.
class MctsSession {
public:
    explicit MctsSession(GameState state, MctsConfig config = {});
    // An explicit perspective remains fixed while the session is re-rooted.
    // This is used by player-specific imperfect-information trees.
    MctsSession(GameState state, PlayerId perspective, MctsConfig config = {});
    ~MctsSession();
    MctsSession(MctsSession&&) noexcept;
    MctsSession& operator=(MctsSession&&) noexcept;
    MctsSession(const MctsSession&) = delete;
    MctsSession& operator=(const MctsSession&) = delete;

    MctsResult run();
    MctsResult run(std::stop_token stopToken, const MctsProgressCallback& progress);
    MctsResult result();

    // Returns true when the action already had a visited subtree. A legal but
    // previously unexplored child is materialized and becomes the new root.
    // A simulator-legal action pruned from search starts a fresh root and
    // returns false; an action illegal in the simulator throws.
    bool advance(const Action& action);
    const GameState& state() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Search entry points for complete GameState and observer InformationState.
// Information search samples complete worlds from the observer's posterior.
// Chance actions
// use Game::chanceOutcomes(); opposing decision nodes optimize against the
// requested perspective. Resignation is deliberately excluded as a dominated
// branch. Non-terminal rollouts use a bounded state evaluator.
class MctsEngine {
public:
    explicit MctsEngine(MctsConfig config = {});

    MctsResult search(const GameState& state) const;
    MctsResult search(const InformationState& state) const;
    MctsResult search(const InformationState& state, std::stop_token stopToken,
                      const MctsProgressCallback& progress) const;
    MctsResult search(const GameState& state, PlayerId perspective) const;
    // Reuses one tree until the iteration limit or stop request. The callback
    // receives immutable snapshots after the first iteration and then every
    // config.progressInterval iterations.
    MctsResult search(const GameState& state, PlayerId perspective,
                      std::stop_token stopToken, const MctsProgressCallback& progress) const;

private:
    MctsConfig config_;
};

// Persistent search using only an observer's InformationState at its boundary.
// Samples worlds according to their posterior and reuses a tree per world.
class InformationMctsSession {
public:
    explicit InformationMctsSession(InformationState state, MctsConfig config = {});
    ~InformationMctsSession();
    InformationMctsSession(InformationMctsSession&&) noexcept;
    InformationMctsSession& operator=(InformationMctsSession&&) noexcept;
    MctsResult run();
    MctsResult run(std::stop_token stopToken, const MctsProgressCallback& progress);
    MctsResult result();
    StepResult transform(const Action& observation);
    // Synchronize to a transformation already applied by the caller.
    bool synchronize(const Action& observation, const InformationState& state);
    const InformationState& state() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace catan
