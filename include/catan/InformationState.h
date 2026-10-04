#pragma once

#include "catan/Game.h"

#include <vector>

namespace catan {

struct InformationWorld {
    GameState state;
    double probability{};
    bool operator==(const InformationWorld&) const = default;
};

// Everything known by one observer. Resources are public in ranked 1v1;
// development hands and deck composition are tracked as joint possibilities.
// The starting position must be a known prior (normally before setup).
class InformationState {
public:
    InformationState(GameState knownPrior, PlayerId observer);

    PlayerId observer() const { return observer_; }
    PlayerId decisionPlayer() const {
        return public_.turn.phase == Phase::Discarding && public_.turn.pendingDiscards[index(observer_)]
            ? observer_ : public_.turn.decisionPlayer;
    }
    std::vector<Action> discardChoices(PlayerId player) const;
    // Display projection only: opponent development cards and deck composition
    // are cleared. This projection is NOT a valid complete simulation state.
    const GameState& state() const { return public_; }
    const std::vector<InformationWorld>& worlds() const { return worlds_; }
    std::size_t worldCount() const { return worlds_.size(); }

    std::vector<Action> legalTransformations() const;
    std::vector<WeightedAction> chanceOutcomes() const;
    StepResult transform(const Action& observation);
    GameState determinize(double quantile) const;
    bool contains(const GameState& world) const;
    double expectedOpponentCards(DevelopmentCard card) const;
    double expectedDeckCards(DevelopmentCard card) const;
    unsigned opponentCardCount() const;
    unsigned deckCardCount() const;
    bool operator==(const InformationState&) const = default;

private:
    PlayerId observer_;
    GameState public_;
    std::vector<InformationWorld> worlds_;
    void refreshPublicState();
};

} // namespace catan
