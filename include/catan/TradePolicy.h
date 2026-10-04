#pragma once

#include "catan/Game.h"

#include <memory>
#include <optional>

namespace catan::search {

// Search-path metadata, never part of the rules or observed game state.
struct TradeContext {
    std::shared_ptr<const GameState> baseline;
    std::vector<MaritimeTrade> trades;
    std::vector<Action> builds;
    bool lastWasBatch{};
    bool operator==(const TradeContext& other) const;
};

struct TradeOptions {
    std::vector<Action> batches;
    bool complete{true}; // False means use normal single trades, not a subset.
};

TradeOptions tradeOptions(const GameState& state, std::size_t maximumStates);
bool redundantTrade(const GameState& state, const TradeContext* context, const Action& action);
std::optional<TradeContext> nextTradeContext(const GameState& before, const TradeContext* context,
                                           const Action& action);

} // namespace catan::search
