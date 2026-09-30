#pragma once

#include "catan/Action.h"

#include <string>
#include <vector>

namespace catan {

bool canAfford(const PlayerState& player, const Resources& cost);
std::uint8_t maritimeRate(const GameState& state, PlayerId player, Resource resource);
bool canBuildSettlement(const GameState& state, PlayerId player, VertexId vertex, bool initial);
bool canBuildRoad(const GameState& state, PlayerId player, EdgeId edge);
bool canBuildCity(const GameState& state, PlayerId player, VertexId vertex);
std::uint8_t longestRoad(const GameState& state, PlayerId player);
bool robberMayTarget(const GameState& state, PlayerId player, HexId hex);
bool validateState(const GameState& state, std::string* reason = nullptr);

} // namespace catan
