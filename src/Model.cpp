#include "catan/Action.h"

#include <sstream>
#include <type_traits>

namespace catan {

const char* toString(PlayerId player) { return player == PlayerId::One ? "Player 1" : "Player 2"; }

const char* toString(Resource resource) {
    constexpr const char* names[] = {"lumber", "brick", "wool", "grain", "ore"};
    return names[index(resource)];
}

const char* toString(Terrain terrain) {
    constexpr const char* names[] = {"forest", "hills", "pasture", "fields", "mountains", "desert"};
    return names[static_cast<std::size_t>(terrain)];
}

const char* toString(DevelopmentCard card) {
    constexpr const char* names[] = {"knight", "road building", "year of plenty", "monopoly", "victory point"};
    return names[index(card)];
}

const char* toString(PortType port) {
    constexpr const char* names[] = {"3:1", "lumber 2:1", "brick 2:1", "wool 2:1", "grain 2:1", "ore 2:1"};
    return names[static_cast<std::size_t>(port)];
}

const char* toString(Phase phase) {
    constexpr const char* names[] = {
        "initial settlement", "initial road", "await roll", "dice roll", "discarding", "move robber",
        "robber steal", "main", "development draw", "free roads", "year of plenty", "finished"
    };
    return names[static_cast<std::size_t>(phase)];
}

std::uint8_t publicVictoryPoints(const GameState& state, PlayerId player) {
    unsigned points = 0;
    for (const auto& building : state.board.buildings) {
        if (building && building->owner == player)
            points += building->type == BuildingType::City ? 2 : 1;
    }
    if (state.awards.longestRoad == player) points += 2;
    if (state.awards.largestArmy == player) points += 2;
    return static_cast<std::uint8_t>(points);
}

std::uint8_t victoryPoints(const GameState& state, PlayerId player) {
    return static_cast<std::uint8_t>(publicVictoryPoints(state, player) +
        state.players[index(player)].developmentCards.cards[index(DevelopmentCard::VictoryPoint)]);
}

std::string toString(const Action& action) {
    return std::visit([](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        std::ostringstream out;
        if constexpr (std::is_same_v<T, PlaceSettlement>) out << "place initial settlement at vertex " << +value.vertex.value;
        else if constexpr (std::is_same_v<T, PlaceRoad>) out << "place initial/free road at edge " << +value.edge.value;
        else if constexpr (std::is_same_v<T, BeginRoll>) out << "roll dice";
        else if constexpr (std::is_same_v<T, RollDice>) out << "roll " << +value.sum;
        else if constexpr (std::is_same_v<T, DiscardResources>) {
            out << "discard [";
            for (std::size_t i = 0; i < ResourceCount; ++i) { if (i) out << ','; out << +value.cards[i]; }
            out << ']';
        } else if constexpr (std::is_same_v<T, ResolveDiscards>) {
            out << "resolve simultaneous discards";
            for (std::size_t p = 0; p < PlayerCount; ++p)
                out << " | " << toString(static_cast<PlayerId>(p)) << ": "
                    << toString(Action{DiscardResources{value.discarded[p]}});
        } else if constexpr (std::is_same_v<T, MoveRobber>) out << "move robber to hex " << +value.hex.value;
        else if constexpr (std::is_same_v<T, StealResource>) out << "steal " << toString(value.resource);
        else if constexpr (std::is_same_v<T, BuildSettlement>) out << "build settlement at vertex " << +value.vertex.value;
        else if constexpr (std::is_same_v<T, BuildRoad>) out << "build road at edge " << +value.edge.value;
        else if constexpr (std::is_same_v<T, BuildCity>) out << "build city at vertex " << +value.vertex.value;
        else if constexpr (std::is_same_v<T, MaritimeTrade>) out << "trade " << toString(value.give) << " for " << toString(value.receive);
        else if constexpr (std::is_same_v<T, MaritimeTradeBatch>) {
            out << "trade batch (" << value.trades.size() << "): ";
            for (std::size_t i = 0; i < value.trades.size(); ++i) {
                if (i) out << "; ";
                out << toString(value.trades[i].give) << " -> " << toString(value.trades[i].receive);
            }
        }
        else if constexpr (std::is_same_v<T, BuyDevelopmentCard>) out << "buy development card";
        else if constexpr (std::is_same_v<T, DrawDevelopmentCard>) out << "draw " << toString(value.card);
        else if constexpr (std::is_same_v<T, DrawUnknownDevelopmentCard>) out << "draw unknown development card";
        else if constexpr (std::is_same_v<T, ObserveVictory>) out << "observe victory for " << toString(value.player);
        else if constexpr (std::is_same_v<T, PlayKnight>) out << "play knight";
        else if constexpr (std::is_same_v<T, PlayRoadBuilding>) out << "play road building";
        else if constexpr (std::is_same_v<T, PlayYearOfPlenty>) out << "play year of plenty";
        else if constexpr (std::is_same_v<T, ChooseYearOfPlentyResource>) out << "choose " << toString(value.resource);
        else if constexpr (std::is_same_v<T, PlayMonopoly>) out << "play monopoly for " << toString(value.resource);
        else if constexpr (std::is_same_v<T, EndTurn>) out << "end turn";
        return out.str();
    }, action);
}

bool isChanceAction(const Action& action) {
    return std::holds_alternative<RollDice>(action) ||
           std::holds_alternative<StealResource>(action) ||
           std::holds_alternative<DrawDevelopmentCard>(action);
}

} // namespace catan
