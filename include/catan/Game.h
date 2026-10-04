#pragma once

#include "catan/Rules.h"

#include <string>
#include <vector>

namespace catan {

struct StepResult {
    bool accepted{};
    std::string message;
    std::vector<std::string> events;
};

class Game {
public:
    explicit Game(std::uint32_t boardSeed = 1);
    explicit Game(BoardState board);
    explicit Game(GameState state);

    const GameState& state() const { return state_; }
    PlayerId decisionPlayer() const { return state_.turn.decisionPlayer; }

    // Joint discard resolutions are validated by transform, not exhaustively
    // enumerated here; use discardChoices to construct ResolveDiscards.
    std::vector<Action> legalTransformations() const;
    // Individual choices, generated from the unchanged pre-discard position.
    // When both players owe cards, submit their choices as ResolveDiscards.
    std::vector<Action> discardChoices(PlayerId player) const;
    // Validate every ordered exchange, not merely the net resource change.
    // Batches are accepted by transform but not exhaustively enumerated by
    // legalTransformations (which retains normal single maritime trades).
    bool canTradeBatch(const MaritimeTradeBatch& batch) const;
    std::vector<WeightedAction> chanceOutcomes() const;
    StepResult transform(const Action& action);

private:
    GameState state_;

    void pay(PlayerId player, const Resources& cost);
    void grantInitialResources(VertexId vertex, PlayerId player, std::vector<std::string>& events);
    void distribute(std::uint8_t roll, std::vector<std::string>& events);
    void prepareDiceDraw();
    void updateDice(std::uint8_t roll);
    void updateAwards();
    void checkWinner(PlayerId player, std::vector<std::string>& events);
    void finishDevelopmentResolution();
    void finishTurn(std::vector<std::string>& events);
    void beginRobber(PlayerId player, Phase returnPhase);
    void finishRobberMove(std::vector<std::string>& events);
    void autoFinishFreeRoads();
};

} // namespace catan
