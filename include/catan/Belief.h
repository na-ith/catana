#pragma once

#include "catan/Game.h"

#include <array>
#include <cstddef>
#include <vector>

namespace catan {

// One exact assignment of the hidden development-card information from a
// player's point of view. The probability is conditional on every observation
// passed to DevelopmentBelief::observe().
struct DevelopmentHypothesis {
    std::array<DevelopmentHand, PlayerCount> hands{};
    DevelopmentDeckState deck{};
    double probability{};
};

// Exact Bayesian tracker for development cards. It deliberately tracks the
// joint hand/deck hypotheses: opponent cards and cards left in the deck are
// correlated and cannot be updated correctly as independent marginals.
class DevelopmentBelief {
public:
    DevelopmentBelief(PlayerId observer, const GameState& initialState);

    PlayerId observer() const { return observer_; }
    const std::vector<DevelopmentHypothesis>& hypotheses() const { return hypotheses_; }
    std::size_t worldCount() const { return hypotheses_.size(); }

    // Updates from one authoritative simulator transition. Draw identities are
    // observed only by the buying player; the other player's belief branches
    // over and marginalizes all feasible draw outcomes.
    void observe(const GameState& before, const Action& action, const GameState& after);

    double probabilityOpponentHas(DevelopmentCard card) const;
    double expectedOpponentCards(DevelopmentCard card) const;
    double expectedDeckCards(DevelopmentCard card) const;

    // Returns a determinization selected by a quantile in [0, 1). Stratified
    // quantiles let a small collection of MCTS workers cover the belief better
    // than unrelated random draws.
    GameState determinize(const GameState& publicState, double quantile) const;
    bool contains(const GameState& state) const;

private:
    PlayerId observer_;
    std::vector<DevelopmentHypothesis> hypotheses_;
};

} // namespace catan
