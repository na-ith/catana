#include "catan/Mcts.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <stdexcept>

namespace catan {
struct InformationMctsSession::Impl {
    InformationState information;
    MctsConfig config;
    std::mt19937 random;
    std::vector<std::unique_ptr<MctsSession>> sessions;
    std::uniform_real_distribution<double> draw{0.0, 1.0};

    Impl(InformationState state, MctsConfig options)
        : information(std::move(state)), config(options), random(options.seed) {
        if (!config.iterations || !config.progressInterval ||
            !std::isfinite(config.explorationConstant) || config.explorationConstant < 0.0)
            throw std::invalid_argument("invalid information MCTS configuration");
        sessionFor(information.determinize(draw(random)));
    }
    MctsSession& sessionFor(const GameState& world) {
        for (auto& session : sessions) if (session->state() == world) return *session;
        auto options = config;
        options.iterations = 1;
        options.seed = random();
        sessions.push_back(std::make_unique<MctsSession>(world, information.observer(), options));
        return *sessions.back();
    }
};

InformationMctsSession::InformationMctsSession(InformationState state, MctsConfig config)
    : impl_(std::make_unique<Impl>(std::move(state), config)) {}
InformationMctsSession::~InformationMctsSession() = default;
InformationMctsSession::InformationMctsSession(InformationMctsSession&&) noexcept = default;
InformationMctsSession& InformationMctsSession::operator=(InformationMctsSession&&) noexcept = default;
const InformationState& InformationMctsSession::state() const { return impl_->information; }

MctsResult InformationMctsSession::result() {
    MctsResult result;
    result.perspective = state().observer();
    result.rootPlayer = state().decisionPlayer();
    const auto chance = state().chanceOutcomes();
    result.chanceNode = !chance.empty();
    std::vector<MctsResult> sampledResults;
    for (auto& session : impl_->sessions) {
        const auto sampled = session->result();
        sampledResults.push_back(sampled);
        result.iterations += sampled.iterations;
        result.maximumDepth = std::max(result.maximumDepth, sampled.maximumDepth);
        result.uniqueRootChildren = std::max(result.uniqueRootChildren, sampled.uniqueRootChildren);
        for (auto evaluation : sampled.actions) {
            if (state().state().turn.phase == Phase::DevelopmentDraw &&
                result.rootPlayer != result.perspective) evaluation.action = DrawUnknownDevelopmentCard{};
            const auto found = std::find_if(result.actions.begin(), result.actions.end(),
                [&](const auto& item) { return item.action == evaluation.action; });
            if (found == result.actions.end()) result.actions.push_back(evaluation);
            else {
                const auto visits = found->visits + evaluation.visits;
                if (visits) found->estimatedWinProbability =
                    (found->estimatedWinProbability * found->visits +
                     evaluation.estimatedWinProbability * evaluation.visits) / visits;
                found->visits = visits;
            }
        }
    }
    const auto legal = state().state().turn.phase == Phase::Discarding
        ? state().discardChoices(state().decisionPlayer()) : state().legalTransformations();
    std::erase_if(result.actions, [&](const auto& item) {
        if (const auto* batch = std::get_if<MaritimeTradeBatch>(&item.action))
            return std::none_of(state().worlds().begin(), state().worlds().end(), [&](const auto& world) {
                return !world.state.winner && Game(world.state).canTradeBatch(*batch);
            });
        return std::find(legal.begin(), legal.end(), item.action) == legal.end();
    });
    for (auto& evaluation : result.actions) {
        evaluation.chanceProbability.reset();
        for (const auto& outcome : chance)
            if (outcome.action == evaluation.action) evaluation.chanceProbability = outcome.probability;
        // Re-rooting can change posterior weights while retaining old visits.
        // Weight conditional values by the current posterior, not historical
        // visit allocation. Unexplored worlds contribute the neutral prior.
        double value = 0.0, mass = 0.0;
        for (const auto& world : state().worlds()) {
            const bool unknown = std::holds_alternative<DrawUnknownDevelopmentCard>(evaluation.action);
            const auto* batch = std::get_if<MaritimeTradeBatch>(&evaluation.action);
            if (world.state.winner) continue;
            if (batch) {
                if (!Game(world.state).canTradeBatch(*batch)) continue;
            } else if (!unknown) {
                const auto legalInWorld = world.state.turn.phase == Phase::Discarding
                    ? Game(world.state).discardChoices(result.rootPlayer)
                    : Game(world.state).legalTransformations();
                if (std::find(legalInWorld.begin(), legalInWorld.end(), evaluation.action) == legalInWorld.end()) continue;
            }
            double weight = world.probability;
            if (result.chanceNode && !unknown) {
                for (const auto& outcome : Game(world.state).chanceOutcomes())
                    if (outcome.action == evaluation.action) weight *= outcome.probability;
            }
            double conditionalValue = 0.5;
            for (std::size_t i = 0; i < impl_->sessions.size(); ++i) {
                if (impl_->sessions[i]->state() != world.state) continue;
                double sum = 0.0;
                std::size_t visits = 0;
                for (const auto& item : sampledResults[i].actions) {
                    if ((!unknown && item.action != evaluation.action) || !item.visits) continue;
                    sum += item.estimatedWinProbability * item.visits;
                    visits += item.visits;
                }
                if (visits) conditionalValue = sum / visits;
                break;
            }
            value += weight * conditionalValue;
            mass += weight;
        }
        if (mass > 0.0) evaluation.estimatedWinProbability = value / mass;
    }
    if (!result.chanceNode) {
        const ActionEvaluation* best = nullptr;
        const bool maximize = result.rootPlayer == result.perspective;
        for (const auto& evaluation : result.actions) {
            if (!evaluation.visits) continue;
            if (!best || (maximize ? evaluation.estimatedWinProbability > best->estimatedWinProbability
                                  : evaluation.estimatedWinProbability < best->estimatedWinProbability)) best = &evaluation;
        }
        if (best) result.bestAction = best->action;
    }
    return result;
}

MctsResult InformationMctsSession::run() { return run({}, {}); }
MctsResult InformationMctsSession::run(std::stop_token stop, const MctsProgressCallback& progress) {
    std::size_t added = 0, published = 0;
    while (added < impl_->config.iterations && !stop.stop_requested()) {
        impl_->sessionFor(state().determinize(impl_->draw(impl_->random))).run();
        ++added;
        if (progress && (added == 1 || added % impl_->config.progressInterval == 0)) {
            progress(result());
            published = added;
        }
    }
    auto value = result();
    if (progress && published != added) progress(value);
    return value;
}

bool InformationMctsSession::synchronize(const Action& observation, const InformationState& updated) {
    if (updated.observer() != state().observer()) throw std::invalid_argument("cannot change observer");
    bool reused = false;
    std::vector<std::unique_ptr<MctsSession>> retained;
    for (auto& session : impl_->sessions) {
        try {
            if (!std::holds_alternative<ObserveVictory>(observation)) {
                Action action = observation;
                if (std::holds_alternative<DrawUnknownDevelopmentCard>(observation)) {
                    const auto chance = Game(session->state()).chanceOutcomes();
                    double target = impl_->draw(impl_->random);
                    if (chance.empty()) continue;
                    action = chance.back().action;
                    for (const auto& outcome : chance) {
                        target -= outcome.probability;
                        if (target <= 0.0) { action = outcome.action; break; }
                    }
                }
                const bool wasReused = session->advance(action);
                if (updated.contains(session->state())) reused |= wasReused;
            }
            if (updated.contains(session->state()) &&
                std::none_of(retained.begin(), retained.end(),
                    [&](const auto& other) { return other->state() == session->state(); }))
                retained.push_back(std::move(session));
        } catch (const std::invalid_argument&) { /* incompatible private world */ }
    }
    impl_->information = updated;
    impl_->sessions = std::move(retained);
    if (impl_->sessions.empty()) impl_->sessionFor(state().determinize(impl_->draw(impl_->random)));
    return reused;
}
StepResult InformationMctsSession::transform(const Action& observation) {
    auto updated = state();
    auto resolution = updated.transform(observation);
    if (resolution.accepted) synchronize(observation, updated);
    return resolution;
}
MctsResult MctsEngine::search(const InformationState& state) const {
    return InformationMctsSession(state, config_).run();
}
MctsResult MctsEngine::search(const InformationState& state, std::stop_token stop,
                            const MctsProgressCallback& progress) const {
    return InformationMctsSession(state, config_).run(stop, progress);
}
} // namespace catan
