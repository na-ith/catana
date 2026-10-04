#include "catan/Belief.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <numeric>
#include <stdexcept>
#include <tuple>

namespace catan {
namespace {

struct HiddenKey {
    std::array<std::uint8_t, DevelopmentCardTypeCount> firstCards{};
    std::array<std::uint8_t, DevelopmentCardTypeCount> firstFresh{};
    std::array<std::uint8_t, DevelopmentCardTypeCount> secondCards{};
    std::array<std::uint8_t, DevelopmentCardTypeCount> secondFresh{};
    std::array<std::uint8_t, DevelopmentCardTypeCount> deck{};
    auto operator<=>(const HiddenKey&) const = default;
};

HiddenKey keyOf(const GameState& state) {
    return {
        state.players[0].developmentCards.cards,
        state.players[0].developmentCards.boughtThisTurn,
        state.players[1].developmentCards.cards,
        state.players[1].developmentCards.boughtThisTurn,
        state.developmentDeck.cards,
    };
}

HiddenKey keyOf(const DevelopmentHypothesis& hypothesis) {
    return {
        hypothesis.hands[0].cards,
        hypothesis.hands[0].boughtThisTurn,
        hypothesis.hands[1].cards,
        hypothesis.hands[1].boughtThisTurn,
        hypothesis.deck.cards,
    };
}

DevelopmentHypothesis hypothesisOf(const GameState& state, double probability) {
    return {{state.players[0].developmentCards, state.players[1].developmentCards},
            state.developmentDeck, probability};
}

GameState applyHidden(GameState state, const DevelopmentHypothesis& hypothesis) {
    state.players[0].developmentCards = hypothesis.hands[0];
    state.players[1].developmentCards = hypothesis.hands[1];
    state.developmentDeck = hypothesis.deck;
    return state;
}

double developmentDrawProbability(const Game& game, const Action& action) {
    for (const auto& outcome : game.chanceOutcomes())
        if (outcome.action == action) return outcome.probability;
    return 0.0;
}

bool samePublicResolution(const GameState& candidate, const GameState& observed) {
    return candidate.turn.phase == observed.turn.phase &&
           candidate.turn.decisionPlayer == observed.turn.decisionPlayer &&
           candidate.winner == observed.winner;
}

} // namespace

DevelopmentBelief::DevelopmentBelief(PlayerId observer, const GameState& initialState)
    : observer_(observer), hypotheses_{hypothesisOf(initialState, 1.0)} {}

void DevelopmentBelief::observe(const GameState& before, const Action& action,
                                const GameState& after) {
    std::map<HiddenKey, DevelopmentHypothesis> merged;
    const bool developmentDraw = std::holds_alternative<DrawDevelopmentCard>(action);
    const PlayerId drawer = before.turn.decisionPlayer;

    for (const auto& hypothesis : hypotheses_) {
        Game possible(applyHidden(before, hypothesis));
        std::vector<std::pair<Action, double>> observations;
        if (developmentDraw && drawer != observer_) {
            for (const auto& outcome : possible.chanceOutcomes())
                if (std::holds_alternative<DrawDevelopmentCard>(outcome.action))
                    observations.emplace_back(outcome.action, outcome.probability);
        } else {
            double likelihood = 1.0;
            if (developmentDraw) likelihood = developmentDrawProbability(possible, action);
            if (likelihood > 0.0) observations.emplace_back(action, likelihood);
        }

        for (const auto& [observedAction, likelihood] : observations) {
            Game branch = possible;
            const auto result = branch.transform(observedAction);
            if (!result.accepted || !samePublicResolution(branch.state(), after)) continue;
            auto candidate = hypothesisOf(branch.state(), hypothesis.probability * likelihood);
            const auto key = keyOf(candidate);
            const auto found = merged.find(key);
            if (found == merged.end()) merged.emplace(key, std::move(candidate));
            else found->second.probability += candidate.probability;
        }
    }

    double totalProbability = 0.0;
    for (const auto& [key, hypothesis] : merged) {
        (void)key;
        totalProbability += hypothesis.probability;
    }
    if (!(totalProbability > 0.0) || !std::isfinite(totalProbability))
        throw std::logic_error("development-card observation eliminated every belief hypothesis");

    hypotheses_.clear();
    hypotheses_.reserve(merged.size());
    for (auto& [key, hypothesis] : merged) {
        (void)key;
        hypothesis.probability /= totalProbability;
        hypotheses_.push_back(std::move(hypothesis));
    }
}

double DevelopmentBelief::probabilityOpponentHas(DevelopmentCard card) const {
    double result = 0.0;
    for (const auto& hypothesis : hypotheses_)
        if (hypothesis.hands[index(opponent(observer_))].cards[index(card)] > 0)
            result += hypothesis.probability;
    return result;
}

double DevelopmentBelief::expectedOpponentCards(DevelopmentCard card) const {
    double result = 0.0;
    for (const auto& hypothesis : hypotheses_)
        result += hypothesis.probability *
                  hypothesis.hands[index(opponent(observer_))].cards[index(card)];
    return result;
}

double DevelopmentBelief::expectedDeckCards(DevelopmentCard card) const {
    double result = 0.0;
    for (const auto& hypothesis : hypotheses_)
        result += hypothesis.probability * hypothesis.deck.cards[index(card)];
    return result;
}

GameState DevelopmentBelief::determinize(const GameState& publicState, double quantile) const {
    if (hypotheses_.empty()) throw std::logic_error("cannot determinize an empty belief");
    quantile = std::clamp(quantile, 0.0, std::nextafter(1.0, 0.0));
    const DevelopmentHypothesis* selected = &hypotheses_.back();
    double cumulative = 0.0;
    for (const auto& hypothesis : hypotheses_) {
        cumulative += hypothesis.probability;
        if (quantile < cumulative) {
            selected = &hypothesis;
            break;
        }
    }
    return applyHidden(publicState, *selected);
}

bool DevelopmentBelief::contains(const GameState& state) const {
    const auto key = keyOf(state);
    return std::any_of(hypotheses_.begin(), hypotheses_.end(),
        [&](const auto& hypothesis) { return keyOf(hypothesis) == key; });
}

} // namespace catan
