#pragma once

#include "catan/State.h"

#include <string>
#include <variant>
#include <vector>

namespace catan {

struct PlaceSettlement { VertexId vertex; bool operator==(const PlaceSettlement&) const = default; };
struct PlaceRoad { EdgeId edge; bool operator==(const PlaceRoad&) const = default; };
struct BeginRoll { bool operator==(const BeginRoll&) const = default; };
struct RollDice { std::uint8_t sum; bool operator==(const RollDice&) const = default; };
struct DiscardResources { Resources cards; bool operator==(const DiscardResources&) const = default; };
// Complete, simultaneous resolution; a zero vector is required for anyone
// who does not owe a discard. DiscardResources is an individual choice.
struct ResolveDiscards { std::array<Resources, PlayerCount> discarded; bool operator==(const ResolveDiscards&) const = default; };
struct MoveRobber { HexId hex; bool operator==(const MoveRobber&) const = default; };
struct StealResource { Resource resource; bool operator==(const StealResource&) const = default; };
struct BuildSettlement { VertexId vertex; bool operator==(const BuildSettlement&) const = default; };
struct BuildRoad { EdgeId edge; bool operator==(const BuildRoad&) const = default; };
struct BuildCity { VertexId vertex; bool operator==(const BuildCity&) const = default; };
struct MaritimeTrade { Resource give; Resource receive; bool operator==(const MaritimeTrade&) const = default; };
// Ordered, atomically validated sequence of otherwise ordinary bank trades.
struct MaritimeTradeBatch { std::vector<MaritimeTrade> trades; bool operator==(const MaritimeTradeBatch&) const = default; };
struct BuyDevelopmentCard { bool operator==(const BuyDevelopmentCard&) const = default; };
struct DrawDevelopmentCard { DevelopmentCard card; bool operator==(const DrawDevelopmentCard&) const = default; };
// Observation-only transformations, accepted by InformationState, not Game.
struct DrawUnknownDevelopmentCard { bool operator==(const DrawUnknownDevelopmentCard&) const = default; };
struct ObserveVictory { PlayerId player; bool operator==(const ObserveVictory&) const = default; };
struct PlayKnight { bool operator==(const PlayKnight&) const = default; };
struct PlayRoadBuilding { bool operator==(const PlayRoadBuilding&) const = default; };
struct PlayYearOfPlenty { bool operator==(const PlayYearOfPlenty&) const = default; };
struct ChooseYearOfPlentyResource { Resource resource; bool operator==(const ChooseYearOfPlentyResource&) const = default; };
struct PlayMonopoly { Resource resource; bool operator==(const PlayMonopoly&) const = default; };
struct EndTurn { bool operator==(const EndTurn&) const = default; };

using Action = std::variant<
    PlaceSettlement, PlaceRoad, BeginRoll, RollDice, DiscardResources, MoveRobber,
    StealResource, BuildSettlement, BuildRoad, BuildCity, MaritimeTrade,
    BuyDevelopmentCard, DrawDevelopmentCard, PlayKnight, PlayRoadBuilding,
    PlayYearOfPlenty, ChooseYearOfPlentyResource, PlayMonopoly, EndTurn,
    DrawUnknownDevelopmentCard, ObserveVictory, ResolveDiscards, MaritimeTradeBatch>;

std::string toString(const Action& action);
bool isChanceAction(const Action& action);

struct WeightedAction {
    Action action;
    double probability{};
};

} // namespace catan
