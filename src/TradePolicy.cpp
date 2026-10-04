#include "catan/TradePolicy.h"

#include <map>

namespace catan::search {
namespace {
bool isBuild(const Action& action) {
    return std::holds_alternative<BuildRoad>(action) ||
           std::holds_alternative<BuildSettlement>(action) ||
           std::holds_alternative<BuildCity>(action);
}
}

bool TradeContext::operator==(const TradeContext& other) const {
    return (baseline == other.baseline || (baseline && other.baseline && *baseline == *other.baseline)) && trades == other.trades &&
           builds == other.builds && lastWasBatch == other.lastWasBatch;
}

TradeOptions tradeOptions(const GameState& state, std::size_t maximumStates) {
    TradeOptions result;
    if (state.turn.phase != Phase::Main) return result;
    if (maximumStates == 0) return {{}, false};
    const auto player = state.turn.decisionPlayer;
    const auto originalHand = state.players[index(player)].resources;
    std::array<std::uint8_t, ResourceCount> rates{};
    for (std::size_t r = 0; r < ResourceCount; ++r)
        rates[r] = maritimeRate(state, player, static_cast<Resource>(r));
    // The bank is determined by the hand: trades conserve each resource.
    // BFS retains a shortest legal sequence for each distinct result, including
    // intermediate results at which the player may stop trading.
    struct Position { Resources hand; std::vector<MaritimeTrade> sequence; };
    std::vector<Position> queue{{originalHand, {}}};
    std::map<Resources, std::size_t> seen{{originalHand, 0}};
    for (std::size_t cursor = 0; cursor < queue.size(); ++cursor) {
        const auto current = queue[cursor]; // Queue may reallocate below.
        for (std::size_t give = 0; give < ResourceCount; ++give) {
            if (current.hand[give] < rates[give]) continue;
            for (std::size_t receive = 0; receive < ResourceCount; ++receive) {
                if (give == receive || state.bank.cards[receive] + originalHand[receive] <= current.hand[receive]) continue;
                auto hand = current.hand;
                hand[give] -= rates[give];
                ++hand[receive];
                if (seen.contains(hand)) continue;
                if (queue.size() >= maximumStates) return {{}, false};
                auto sequence = current.sequence;
                sequence.push_back({static_cast<Resource>(give), static_cast<Resource>(receive)});
                seen.emplace(hand, queue.size());
                queue.push_back({hand, sequence});
                result.batches.emplace_back(MaritimeTradeBatch{std::move(sequence)});
            }
        }
    }
    return result;
}

bool redundantTrade(const GameState& state, const TradeContext* context, const Action& action) {
    if (!context || state.turn.phase != Phase::Main) return false;
    MaritimeTradeBatch candidate;
    if (const auto* batch = std::get_if<MaritimeTradeBatch>(&action)) {
        if (context->lastWasBatch) return true; // Already represented by a longer batch.
        candidate = *batch;
    } else if (const auto* single = std::get_if<MaritimeTrade>(&action)) {
        candidate.trades.push_back(*single); // Enumeration-budget fallback.
    } else return false;
    if (context->builds.empty()) return false;

    Game reordered(*context->baseline);
    auto combined = context->trades;
    combined.insert(combined.end(), candidate.trades.begin(), candidate.trades.end());
    if (!reordered.transform(MaritimeTradeBatch{std::move(combined)}).accepted) return false;
    for (const auto& build : context->builds)
        if (!reordered.transform(build).accepted) return false;
    Game original(state);
    return original.transform(candidate).accepted && original.state() == reordered.state();
}

std::optional<TradeContext> nextTradeContext(const GameState& before, const TradeContext* context,
                                           const Action& action) {
    if (before.turn.phase != Phase::Main) return std::nullopt;
    const auto* batch = std::get_if<MaritimeTradeBatch>(&action);
    const auto* single = std::get_if<MaritimeTrade>(&action);
    if (!batch && !single && !isBuild(action)) return std::nullopt;
    TradeContext next = context ? *context : TradeContext{std::make_shared<GameState>(before), {}, {}, false};
    // A non-commuting trade follows a real ordering barrier (e.g. a new port
    // or bank replenishment). Start a new segment after those earlier builds.
    if ((batch || single) && !next.builds.empty())
        next = {std::make_shared<GameState>(before), {}, {}, false};
    if (batch) next.trades.insert(next.trades.end(), batch->trades.begin(), batch->trades.end());
    else if (single) next.trades.push_back(*single);
    else next.builds.push_back(action);
    next.lastWasBatch = batch != nullptr;
    return next;
}

} // namespace catan::search
