#pragma once

#include "catan/Board.h"

#include <array>
#include <optional>

namespace catan {

struct DevelopmentHand {
    std::array<std::uint8_t, DevelopmentCardTypeCount> cards{};
    std::array<std::uint8_t, DevelopmentCardTypeCount> boughtThisTurn{};
    bool operator==(const DevelopmentHand&) const = default;
};

struct PlayerState {
    Resources resources{};
    DevelopmentHand developmentCards{};
    std::uint8_t playedKnights{};
    bool operator==(const PlayerState&) const = default;
};

struct ResourceBank {
    Resources cards{19, 19, 19, 19, 19};
    bool operator==(const ResourceBank&) const = default;
};

struct DevelopmentDeckState {
    std::array<std::uint8_t, DevelopmentCardTypeCount> cards{14, 2, 2, 2, 5};
    bool operator==(const DevelopmentDeckState&) const = default;
};

struct BalancedDiceState {
    std::array<std::uint8_t, 13> remainingBySum{};
    std::array<std::uint8_t, 13> recentCounts{};
    std::array<std::uint8_t, 5> recentRolls{};
    std::uint8_t recentRollCount{};
    std::uint8_t cardsLeft{};
    std::array<std::uint16_t, PlayerCount> sevensByPlayer{};
    std::optional<PlayerId> sevenStreakPlayer{};
    std::uint8_t sevenStreakCount{};
    bool operator==(const BalancedDiceState&) const = default;
};

enum class Phase : std::uint8_t {
    InitialSettlement,
    InitialRoad,
    AwaitRoll,
    DiceRoll,
    Discarding,
    MoveRobber,
    RobberSteal,
    Main,
    DevelopmentDraw,
    FreeRoads,
    YearOfPlenty,
    Finished
};

struct TurnState {
    Phase phase{Phase::InitialSettlement};
    Phase returnPhase{Phase::Main};
    std::array<std::uint8_t, PlayerCount> pendingDiscards{};
    PlayerId decisionPlayer{PlayerId::One};
    std::optional<VertexId> pendingInitialSettlement{};
    std::uint8_t placementIndex{};
    std::uint8_t freeRoadsRemaining{};
    std::uint8_t plentyPicksRemaining{};
    bool playedDevelopmentCard{};
    bool operator==(const TurnState&) const = default;
};

struct AwardState {
    std::optional<PlayerId> longestRoad{};
    std::optional<PlayerId> largestArmy{};
    std::array<std::uint8_t, PlayerCount> roadLengths{};
    bool operator==(const AwardState&) const = default;
};

struct GameState {
    BoardState board{};
    std::array<PlayerState, PlayerCount> players{};
    ResourceBank bank{};
    DevelopmentDeckState developmentDeck{};
    BalancedDiceState dice{};
    PlayerId activePlayer{PlayerId::One};
    TurnState turn{};
    AwardState awards{};
    std::uint32_t turnNumber{};
    std::optional<PlayerId> winner{};
    bool operator==(const GameState&) const = default;
};

std::uint8_t publicVictoryPoints(const GameState& state, PlayerId player);
std::uint8_t victoryPoints(const GameState& state, PlayerId player);
const char* toString(Phase phase);

} // namespace catan
