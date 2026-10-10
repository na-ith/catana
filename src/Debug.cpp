#include "catan/Debug.h"

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>

#if !defined(_WIN32)
#include <csignal>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef CATAN_RENDER_SCRIPT
#define CATAN_RENDER_SCRIPT "tools/render_state.py"
#endif

#ifndef CATAN_PYTHON_EXECUTABLE
#define CATAN_PYTHON_EXECUTABLE "python3"
#endif

namespace catan::debug {
namespace {

struct LiveControls {
    std::uint64_t positionVersion{};
    bool canRewind{};
    std::array<bool, PlayerCount> treeRunning{true, true};
    std::optional<PlayerId> discardPlayer;
    std::array<bool, PlayerCount> discardSelected{};
    const std::vector<ViewerEvent>* events{};
};

std::string shellQuote(const std::string& value) {
    std::string result{"'"};
    for (char character : value) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + '\'';
}

template <typename Values>
void writeNumbers(std::ostream& out, const Values& values) {
    out << '[';
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) out << ',';
        out << +values[i];
    }
    out << ']';
}

void writeResources(std::ostream& out, const Resources& resources) {
    out << '{';
    for (std::size_t i = 0; i < ResourceCount; ++i) {
        if (i) out << ',';
        out << '\"' << toString(static_cast<Resource>(i)) << "\":" << +resources[i];
    }
    out << '}';
}

void writeDevelopmentCards(std::ostream& out,
                           const std::array<std::uint8_t, DevelopmentCardTypeCount>& cards) {
    out << '{';
    for (std::size_t i = 0; i < DevelopmentCardTypeCount; ++i) {
        if (i) out << ',';
        out << '\"' << toString(static_cast<DevelopmentCard>(i)) << "\":" << +cards[i];
    }
    out << '}';
}

void writeOptionalPlayer(std::ostream& out, std::optional<PlayerId> player) {
    if (player) out << index(*player);
    else out << "null";
}

void writeJsonString(std::ostream& out, const std::string& value) {
    out << '"';
    for (const char character : value) {
        switch (character) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default: out << character; break;
        }
    }
    out << '"';
}

void writeHighlightTargets(std::ostream& out, const Action& action, PlayerId perspective) {
    std::vector<std::string> targets;
    const auto resourceTarget = [&](PlayerId player, Resource resource) {
        return "resource-" + std::to_string(index(player)) + "-" + toString(resource);
    };
    const auto cardName = [](DevelopmentCard card) {
        std::string name = toString(card);
        for (auto& character : name) if (character == ' ') character = '-';
        return name;
    };
    const auto developmentCardTarget = [&](PlayerId player, DevelopmentCard card) {
        return "devcard-" + std::to_string(index(player)) + "-" + cardName(card);
    };
    const auto addResources = [&](PlayerId player, const Resources& resources) {
        for (std::size_t resource = 0; resource < ResourceCount; ++resource) {
            if (resources[resource] != 0)
                targets.push_back(resourceTarget(player, static_cast<Resource>(resource)));
        }
    };

    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PlaceSettlement> ||
                      std::is_same_v<T, BuildSettlement> ||
                      std::is_same_v<T, BuildCity>) {
            targets.push_back("vertex-" + std::to_string(value.vertex.value));
        } else if constexpr (std::is_same_v<T, PlaceRoad> ||
                             std::is_same_v<T, BuildRoad>) {
            targets.push_back("edge-" + std::to_string(value.edge.value));
        } else if constexpr (std::is_same_v<T, MoveRobber>) {
            targets.push_back("hex-token-" + std::to_string(value.hex.value));
        } else if constexpr (std::is_same_v<T, BeginRoll> ||
                             std::is_same_v<T, RollDice>) {
            targets.push_back("dice");
        } else if constexpr (std::is_same_v<T, DiscardResources>) {
            addResources(perspective, value.cards);
        } else if constexpr (std::is_same_v<T, StealResource>) {
            const auto victim = perspective == PlayerId::One ? PlayerId::Two : PlayerId::One;
            targets.push_back(resourceTarget(victim, value.resource));
        } else if constexpr (std::is_same_v<T, MaritimeTrade>) {
            targets.push_back(resourceTarget(perspective, value.give));
            targets.push_back(resourceTarget(perspective, value.receive));
            targets.push_back("bank");
        } else if constexpr (std::is_same_v<T, MaritimeTradeBatch>) {
            for (const auto& trade : value.trades) {
                targets.push_back(resourceTarget(perspective, trade.give));
                targets.push_back(resourceTarget(perspective, trade.receive));
            }
            targets.push_back("bank");
        } else if constexpr (std::is_same_v<T, BuyDevelopmentCard>) {
            targets.push_back("development-deck");
        } else if constexpr (std::is_same_v<T, DrawDevelopmentCard>) {
            targets.push_back("development-deck");
            targets.push_back("development-deck-card-" + cardName(value.card));
        } else if constexpr (std::is_same_v<T, PlayKnight>) {
            targets.push_back(developmentCardTarget(perspective, DevelopmentCard::Knight));
        } else if constexpr (std::is_same_v<T, PlayRoadBuilding>) {
            targets.push_back(developmentCardTarget(perspective, DevelopmentCard::RoadBuilding));
        } else if constexpr (std::is_same_v<T, PlayYearOfPlenty>) {
            targets.push_back(developmentCardTarget(perspective, DevelopmentCard::YearOfPlenty));
        } else if constexpr (std::is_same_v<T, ChooseYearOfPlentyResource>) {
            targets.push_back(developmentCardTarget(perspective, DevelopmentCard::YearOfPlenty));
            targets.push_back(resourceTarget(perspective, value.resource));
        } else if constexpr (std::is_same_v<T, PlayMonopoly>) {
            targets.push_back(developmentCardTarget(perspective, DevelopmentCard::Monopoly));
            const auto victim = perspective == PlayerId::One ? PlayerId::Two : PlayerId::One;
            targets.push_back(resourceTarget(victim, value.resource));
        } else if constexpr (std::is_same_v<T, EndTurn> || std::is_same_v<T, Resign>) {
            targets.push_back("turn");
        }
    }, action);

    out << '[';
    for (std::size_t i = 0; i < targets.size(); ++i) {
        if (i) out << ',';
        writeJsonString(out, targets[i]);
    }
    out << ']';
}

const char* actionKind(const Action& action) {
    static constexpr std::array kinds{
        "place_settlement", "place_road", "begin_roll", "roll_dice", "discard",
        "move_robber", "steal", "build_settlement", "build_road", "build_city", "trade",
        "buy_development", "draw_development", "play_knight", "play_road_building",
        "play_year_of_plenty", "choose_resource", "play_monopoly", "end_turn", "resign",
        "draw_unknown_development", "observe_victory", "resolve_discards", "trade_batch"
    };
    static_assert(kinds.size() == std::variant_size_v<Action>);
    return kinds[action.index()];
}

// Serialize parameters from the variant, never by parsing display descriptions.
void writeActionParameters(std::ostream& out, const Action& action) {
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, PlaceSettlement> || std::is_same_v<T, BuildSettlement> ||
                      std::is_same_v<T, BuildCity>) out << ",\"vertex\":" << +value.vertex.value;
        else if constexpr (std::is_same_v<T, PlaceRoad> || std::is_same_v<T, BuildRoad>)
            out << ",\"edge\":" << +value.edge.value;
        else if constexpr (std::is_same_v<T, MoveRobber>) out << ",\"hex\":" << +value.hex.value;
        else if constexpr (std::is_same_v<T, RollDice>) out << ",\"sum\":" << +value.sum;
        else if constexpr (std::is_same_v<T, DiscardResources>) {
            out << ",\"cards\":"; writeNumbers(out, value.cards);
        } else if constexpr (std::is_same_v<T, ResolveDiscards>) {
            out << ",\"discarded\":[";
            writeNumbers(out, value.discarded[0]); out << ','; writeNumbers(out, value.discarded[1]); out << ']';
        } else if constexpr (std::is_same_v<T, MaritimeTrade>) {
            out << ",\"give\":"; writeJsonString(out, toString(value.give));
            out << ",\"receive\":"; writeJsonString(out, toString(value.receive));
        } else if constexpr (std::is_same_v<T, MaritimeTradeBatch>) {
            out << ",\"trades\":[";
            for (std::size_t i = 0; i < value.trades.size(); ++i) {
                if (i) out << ',';
                out << "{\"give\":"; writeJsonString(out, toString(value.trades[i].give));
                out << ",\"receive\":"; writeJsonString(out, toString(value.trades[i].receive)); out << '}';
            }
            out << ']';
        } else if constexpr (std::is_same_v<T, StealResource> || std::is_same_v<T, ChooseYearOfPlentyResource> ||
                             std::is_same_v<T, PlayMonopoly>) {
            out << ",\"resource\":"; writeJsonString(out, toString(value.resource));
        } else if constexpr (std::is_same_v<T, DrawDevelopmentCard>) {
            out << ",\"card\":"; writeJsonString(out, toString(value.card));
        } else if constexpr (std::is_same_v<T, ObserveVictory>) out << ",\"player\":" << index(value.player);
    }, action);
}

std::string moveId(const Action& action) {
    std::ostringstream parameters;
    writeActionParameters(parameters, action);
    return std::string(actionKind(action)) + parameters.str();
}

void writeActionFields(std::ostream& out, const Action& action, PlayerId actor) {
    out << "\"id\":"; writeJsonString(out, moveId(action));
    out << ",\"type\":"; writeJsonString(out, actionKind(action));
    out << ",\"description\":"; writeJsonString(out, toString(action));
    writeActionParameters(out, action);
    out << ",\"highlight_targets\":"; writeHighlightTargets(out, action, actor);
}

void writeAnalysis(std::ostream& out, const MctsResult* analysis, bool compact = false,
                   const std::vector<std::string>* ids = nullptr) {
    if (!analysis) { out << "null"; return; }
    std::optional<std::size_t> bestIndex;
    if (analysis->bestAction) {
        for (std::size_t i = 0; i < analysis->actions.size(); ++i)
            if (analysis->actions[i].action == *analysis->bestAction) { bestIndex = i; break; }
    }
    out << "{\"perspective\":" << index(analysis->perspective)
        << ",\"root_player\":" << index(analysis->rootPlayer)
        << ",\"iterations\":" << analysis->iterations
        << ",\"maximum_depth\":" << analysis->maximumDepth
        << ",\"workers\":" << analysis->workerCount
        << ",\"unique_root_children\":" << analysis->uniqueRootChildren
        << ",\"chance_node\":" << (analysis->chanceNode ? "true" : "false")
        << ",\"best_action_index\":";
    if (bestIndex) out << *bestIndex; else out << "null";
    out << ",\"actions\":[" << std::setprecision(17);
    for (std::size_t i = 0; i < analysis->actions.size(); ++i) {
        if (i) out << ',';
        const auto& evaluation = analysis->actions[i];
        out << "{\"index\":" << i << ',';
        if (compact) { out << "\"id\":"; writeJsonString(out, ids ? (*ids)[i] : moveId(evaluation.action)); }
        else writeActionFields(out, evaluation.action, analysis->perspective);
        out << ",\"visits\":" << evaluation.visits
            << ",\"win_probability\":" << evaluation.estimatedWinProbability
            << ",\"chance_probability\":";
        if (evaluation.chanceProbability) out << *evaluation.chanceProbability; else out << "null";
        out << ",\"recommended\":" << (bestIndex && *bestIndex == i ? "true" : "false") << '}';
    }
    out << "]}";
}

void writeTopology(std::ostream& out) {
    const auto& topology = BoardTopology::standard();
    out << "\"topology\":{\"vertices\":[";
    for (std::size_t v = 0; v < topology.vertices.size(); ++v) {
        if (v) out << ',';
        const auto& vertex = topology.vertices[v];
        out << "{\"id\":" << v << ",\"x\":" << vertex.position.x << ",\"y\":" << vertex.position.y << '}';
    }
    out << "],\"edges\":[";
    for (std::size_t e = 0; e < topology.edges.size(); ++e) {
        if (e) out << ',';
        const auto& edge = topology.edges[e];
        out << "{\"id\":" << e << ",\"vertices\":[" << +edge.vertices[0].value << ',' << +edge.vertices[1].value << "]}";
    }
    out << "],\"hexes\":[";
    for (std::size_t h = 0; h < topology.hexes.size(); ++h) {
        if (h) out << ',';
        const auto& hex = topology.hexes[h];
        out << "{\"id\":" << h << ",\"x\":" << hex.center.x << ",\"y\":" << hex.center.y << ",\"vertices\":[";
        for (std::size_t corner = 0; corner < hex.vertices.size(); ++corner) {
            if (corner) out << ',';
            out << +hex.vertices[corner].value;
        }
        out << "]}";
    }
    out << "]},\n";
}

void writeBoardLayout(std::ostream& out, const GameState& state, bool includeRobber) {
    out << "\"hexes\":[";
    for (std::size_t h = 0; h < state.board.hexes.size(); ++h) {
        if (h) out << ',';
        const auto& hex = state.board.hexes[h];
        out << "{\"id\":" << h << ",\"terrain\":\"" << toString(hex.terrain)
            << "\",\"number\":" << +hex.number;
        if (includeRobber) out << ",\"robber\":" << (state.board.robberHex.value == h ? "true" : "false");
        out << '}';
    }
    out << "],\"ports\":[";
    for (std::size_t p = 0; p < state.board.ports.size(); ++p) {
        if (p) out << ',';
        const auto& port = state.board.ports[p];
        out << "{\"edge\":" << +port.edge.value << ",\"type\":\"" << toString(port.type) << "\"}";
    }
    out << "]";
}

template <typename Writer>
bool publishJson(const std::filesystem::path& path, Writer writer, std::string* error = nullptr) {
    const auto next = std::filesystem::path(path.string() + ".next");
    std::ofstream out(next);
    writer(out);
    out.close();
    if (!out) { if (error) *error = "could not write " + next.string(); return false; }
    std::error_code filesystemError;
    std::filesystem::rename(next, path, filesystemError);
    if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
    return true;
}

bool writeStateJsonImpl(const GameState& state, const MctsResult* analysis, const LiveControls* controls,
                        const std::filesystem::path& output, std::string* error,
                        const InformationState* information = nullptr,
                        bool includeLayout = true, const std::vector<Action>* legalActions = nullptr) {
    std::error_code directoryError;
    if (output.has_parent_path()) std::filesystem::create_directories(output.parent_path(), directoryError);
    if (directoryError) {
        if (error) *error = "could not create output directory: " + directoryError.message();
        return false;
    }
    std::ofstream out(output);
    if (!out) {
        if (error) *error = "could not open " + output.string();
        return false;
    }

    out << "{\n\"schema\":\"" << (includeLayout ? "catan-debug-state-v1" : "catan-viewer-position-v1")
        << "\",\n\"controls\":";
    if (controls) {
        out << "{\"position_version\":" << controls->positionVersion
            << ",\"can_rewind\":" << (controls->canRewind ? "true" : "false")
            << ",\"tree_running\":["
            << (controls->treeRunning[0] ? "true" : "false") << ','
            << (controls->treeRunning[1] ? "true" : "false") << "]"
            << ",\"discard_input_player\":";
        if (controls->discardPlayer) out << index(*controls->discardPlayer);
        else out << "null";
        out << ",\"discard_selected\":[" << (controls->discardSelected[0] ? "true" : "false")
            << ',' << (controls->discardSelected[1] ? "true" : "false") << "]},\n";
    } else {
        out << "null,\n";
    }
    if (information) {
        out << "\"information\":{\"observer\":" << index(information->observer())
            << ",\"world_count\":" << information->worldCount()
            << ",\"opponent_card_count\":" << information->opponentCardCount()
            << ",\"deck_count\":" << information->deckCardCount();
        for (const bool deck : {false, true}) {
            out << (deck ? ",\"expected_deck_cards\":{" : ",\"expected_opponent_cards\":{");
            for (std::size_t c = 0; c < DevelopmentCardTypeCount; ++c) {
                if (c) out << ',';
                const auto card = static_cast<DevelopmentCard>(c);
                writeJsonString(out, toString(card));
                out << ':' << std::setprecision(17) << (deck ? information->expectedDeckCards(card)
                                                            : information->expectedOpponentCards(card));
            }
            out << '}';
        }
        out << "},\n";
    }
    out << "\"turn\":{\"number\":" << state.turnNumber
        << ",\"phase\":\"" << toString(state.turn.phase) << "\""
        << ",\"active_player\":" << index(state.activePlayer)
        << ",\"decision_player\":" << index(state.turn.decisionPlayer)
        << ",\"return_phase\":\"" << toString(state.turn.returnPhase) << "\""
        << ",\"winner\":";
    writeOptionalPlayer(out, state.winner);
    out << ",\"played_development_card\":" << (state.turn.playedDevelopmentCard ? "true" : "false")
        << ",\"pending_discards\":";
    writeNumbers(out, state.turn.pendingDiscards);
    out << ",\"pending_initial_vertex\":";
    if (state.turn.pendingInitialSettlement) out << +state.turn.pendingInitialSettlement->value;
    else out << "null";
    out << ",\"placement_index\":" << +state.turn.placementIndex
        << ",\"free_roads_remaining\":" << +state.turn.freeRoadsRemaining
        << ",\"plenty_picks_remaining\":" << +state.turn.plentyPicksRemaining
        << "},\n";

    if (includeLayout) writeTopology(out);
    out << "\"board\":{\"robber_hex\":" << +state.board.robberHex.value;
    if (includeLayout) {
        out << ',';
        writeBoardLayout(out, state, true);
    }
    out << ",\"buildings\":[";
    for (std::size_t v = 0; v < state.board.buildings.size(); ++v) {
        if (v) out << ',';
        const auto& building = state.board.buildings[v];
        if (!building) out << "null";
        else out << "{\"owner\":" << index(building->owner) << ",\"type\":\""
                 << (building->type == BuildingType::City ? "city" : "settlement") << "\"}";
    }
    out << "],\"roads\":[";
    for (std::size_t e = 0; e < state.board.roads.size(); ++e) {
        if (e) out << ',';
        const auto& road = state.board.roads[e];
        if (!road) out << "null";
        else out << "{\"owner\":" << index(road->owner) << '}';
    }
    out << "]},\n";

    out << "\"players\":[";
    for (std::size_t p = 0; p < PlayerCount; ++p) {
        if (p) out << ',';
        const auto player = static_cast<PlayerId>(p);
        const auto& value = state.players[p];
        out << "{\"id\":" << p << ",\"resources\":";
        writeResources(out, value.resources);
        out << ",\"development_cards\":";
        if (information && player != information->observer()) out << "null";
        else writeDevelopmentCards(out, value.developmentCards.cards);
        out << ",\"bought_this_turn\":";
        if (information && player != information->observer()) out << "null";
        else writeDevelopmentCards(out, value.developmentCards.boughtThisTurn);
        out << ",\"played_knights\":" << +value.playedKnights
            << ",\"public_vp\":" << +publicVictoryPoints(state, player)
            << ",\"total_vp\":";
        if (information && player != information->observer()) out << "null";
        else out << +victoryPoints(state, player);
        out << '}';
    }
    out << "],\n\"bank\":";
    writeResources(out, state.bank.cards);
    out << ",\n\"development_deck\":";
    if (information) out << "null";
    else writeDevelopmentCards(out, state.developmentDeck.cards);
    out << ",\n\"dice\":{\"cards_left\":" << +state.dice.cardsLeft
        << ",\"remaining_by_sum\":";
    writeNumbers(out, state.dice.remainingBySum);
    out << ",\"recent_counts\":";
    writeNumbers(out, state.dice.recentCounts);
    out << ",\"recent_rolls\":[";
    for (std::size_t i = 0; i < state.dice.recentRollCount; ++i) {
        if (i) out << ',';
        out << +state.dice.recentRolls[i];
    }
    out << "],\"sevens_by_player\":";
    writeNumbers(out, state.dice.sevensByPlayer);
    out << ",\"seven_streak_player\":";
    writeOptionalPlayer(out, state.dice.sevenStreakPlayer);
    out << ",\"seven_streak_count\":" << +state.dice.sevenStreakCount
        << "},\n\"awards\":{\"longest_road\":";
    writeOptionalPlayer(out, state.awards.longestRoad);
    out << ",\"largest_army\":";
    writeOptionalPlayer(out, state.awards.largestArmy);
    out << ",\"road_lengths\":";
    writeNumbers(out, state.awards.roadLengths);
    out << "},\n\"transformations\":[";
    auto transformations = legalActions ? *legalActions
        : information ? information->legalTransformations() : Game(state).legalTransformations();
    if (!legalActions && state.turn.phase == Phase::Discarding) {
        const auto actor = controls && controls->discardPlayer ? *controls->discardPlayer
            : information ? information->decisionPlayer() : state.turn.decisionPlayer;
        transformations = information ? information->discardChoices(actor) : Game(state).discardChoices(actor);
    }
    if (!legalActions) std::erase_if(transformations, [](const Action& action) { return std::holds_alternative<Resign>(action); });
    const auto chances = information ? information->chanceOutcomes() : Game(state).chanceOutcomes();
    for (std::size_t i = 0; i < transformations.size(); ++i) {
        if (i) out << ',';
        out << "{\"index\":" << i << ',';
        const auto actor = controls && controls->discardPlayer ? *controls->discardPlayer : state.turn.decisionPlayer;
        writeActionFields(out, transformations[i], actor);
        if (const auto* trade = std::get_if<MaritimeTrade>(&transformations[i]))
            out << ",\"give_amount\":" << +maritimeRate(state, actor, trade->give);
        out << ",\"chance_probability\":";
        const auto found = std::find_if(chances.begin(), chances.end(),
            [&](const auto& outcome) { return outcome.action == transformations[i]; });
        if (found == chances.end()) out << "null";
        else out << std::setprecision(17) << found->probability;
        out << '}';
    }
    out << "],\n\"possible_winners\":[";
    bool firstWinner = true;
    if (information && !state.winner) {
        for (std::size_t p = 0; p < PlayerCount; ++p) {
            const auto player = static_cast<PlayerId>(p);
            if (std::any_of(information->worlds().begin(), information->worlds().end(),
                           [&](const auto& world) { return world.state.winner == player; })) {
                if (!firstWinner) out << ',';
                out << p;
                firstWinner = false;
            }
        }
    }
    out << "],\n\"mcts\":";
    writeAnalysis(out, analysis);
    if (controls && controls->events) {
        out << ",\n\"events\":[";
        for (std::size_t i = 0; i < controls->events->size(); ++i) {
            if (i) out << ',';
            const auto& event = (*controls->events)[i];
            out << "{\"id\":" << event.id << ",\"type\":"; writeJsonString(out, event.type);
            out << ",\"message\":"; writeJsonString(out, event.message);
            out << ",\"details\":[";
            for (std::size_t d = 0; d < event.details.size(); ++d) {
                if (d) out << ',';
                writeJsonString(out, event.details[d]);
            }
            out << ']';
            if (event.diceTotal) {
                out << ",\"dice_total\":" << +*event.diceTotal << ",\"production\":[";
                writeResources(out, event.production[0]); out << ','; writeResources(out, event.production[1]);
                out << ']';
            }
            out << '}';
        }
        out << ']';
    }
    out << "\n}\n";

    if (!out) {
        if (error) *error = "failed while writing " + output.string();
        return false;
    }
    return true;
}

bool renderStateImageImpl(const GameState& state, const MctsResult* analysis,
                          const std::filesystem::path& output, std::string* error,
                          const InformationState* information = nullptr) {
    if (output.extension() != ".svg") {
        if (error) *error = "debug renderer output must have an .svg extension";
        return false;
    }
    std::error_code filesystemError;
    const auto absoluteOutput = std::filesystem::absolute(output, filesystemError);
    if (filesystemError) {
        if (error) *error = filesystemError.message();
        return false;
    }
    const auto snapshot = absoluteOutput.string() + ".json.tmp";
    const bool wroteSnapshot = writeStateJsonImpl(state, analysis, nullptr, snapshot, error, information);
    if (!wroteSnapshot) return false;

    const std::string command = shellQuote(CATAN_PYTHON_EXECUTABLE) + " " +
        shellQuote(CATAN_RENDER_SCRIPT) + " --input " + shellQuote(snapshot) +
        " --output " + shellQuote(absoluteOutput.string());
    const int status = std::system(command.c_str());
    std::filesystem::remove(snapshot, filesystemError);
    if (status != 0) {
        if (error) *error = "renderer exited with status " + std::to_string(status);
        return false;
    }
    return true;
}

} // namespace

ViewerEvent describeViewerEvent(const Action& action, const GameState& before,
                               const GameState& after, const std::vector<std::string>& engineEvents) {
    ViewerEvent event;
    event.type = actionKind(action);
    const auto playerName = [](PlayerId player) { return "P" + std::to_string(index(player) + 1); };
    const auto actor = playerName(before.turn.decisionPlayer);
    std::visit([&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, RollDice>) {
            event.diceTotal = value.sum;
            event.message = actor + " rolled " + std::to_string(value.sum);
        } else if constexpr (std::is_same_v<T, PlaceSettlement>) event.message = actor + " placed a starting settlement";
        else if constexpr (std::is_same_v<T, PlaceRoad>) event.message = actor + " placed a road";
        else if constexpr (std::is_same_v<T, BuildSettlement>) event.message = actor + " built a settlement";
        else if constexpr (std::is_same_v<T, BuildRoad>) event.message = actor + " built a road";
        else if constexpr (std::is_same_v<T, BuildCity>) event.message = actor + " upgraded a settlement to a city";
        else if constexpr (std::is_same_v<T, BeginRoll>) event.message = actor + " is rolling the dice";
        else if constexpr (std::is_same_v<T, EndTurn>) event.message = actor + " ended their turn";
        else if constexpr (std::is_same_v<T, MoveRobber>) event.message = actor + " moved the robber";
        else if constexpr (std::is_same_v<T, StealResource>)
            event.message = actor + " stole 1 " + toString(value.resource) + " from " + playerName(opponent(before.turn.decisionPlayer));
        else if constexpr (std::is_same_v<T, MaritimeTrade>) event.message = actor + " traded " +
            std::to_string(maritimeRate(before, before.turn.decisionPlayer, value.give)) + " " + toString(value.give) + " for 1 " + toString(value.receive);
        else if constexpr (std::is_same_v<T, MaritimeTradeBatch>) {
            event.message = actor + " made " + std::to_string(value.trades.size()) + " bank trades";
            for (const auto& trade : value.trades) event.details.push_back(
                std::to_string(maritimeRate(before, before.turn.decisionPlayer, trade.give)) + " " + toString(trade.give) + " → 1 " + toString(trade.receive));
        } else if constexpr (std::is_same_v<T, BuyDevelopmentCard>) event.message = actor + " bought a development card";
        else if constexpr (std::is_same_v<T, DrawDevelopmentCard>) event.message = actor + " received " + toString(value.card);
        else if constexpr (std::is_same_v<T, DrawUnknownDevelopmentCard>) event.message = actor + " received an unknown development card";
        else if constexpr (std::is_same_v<T, PlayKnight>) event.message = actor + " played a Knight";
        else if constexpr (std::is_same_v<T, PlayRoadBuilding>) event.message = actor + " played Road Building";
        else if constexpr (std::is_same_v<T, PlayYearOfPlenty>) event.message = actor + " played Year of Plenty";
        else if constexpr (std::is_same_v<T, ChooseYearOfPlentyResource>) event.message = actor + " took 1 " + toString(value.resource) + " from the bank";
        else if constexpr (std::is_same_v<T, PlayMonopoly>) event.message = actor + " played Monopoly for " + toString(value.resource);
        else if constexpr (std::is_same_v<T, DiscardResources>) event.message = actor + " discarded resources";
        else if constexpr (std::is_same_v<T, ResolveDiscards>) event.message = "Discard choices resolved together";
        else if constexpr (std::is_same_v<T, ObserveVictory>) event.message = playerName(value.player) + " won the game";
        else if constexpr (std::is_same_v<T, Resign>) event.message = actor + " resigned";
    }, action);
    for (std::size_t p = 0; p < PlayerCount; ++p) {
        std::string gains, losses;
        for (std::size_t r = 0; r < ResourceCount; ++r) {
            const auto difference = static_cast<int>(after.players[p].resources[r]) - before.players[p].resources[r];
            if (!difference) continue;
            auto& description = difference > 0 ? gains : losses;
            if (!description.empty()) description += ", ";
            description += std::to_string(difference > 0 ? difference : -difference) + " " + toString(static_cast<Resource>(r));
            if (difference > 0 && event.diceTotal) event.production[p][r] = static_cast<std::uint8_t>(difference);
        }
        const auto name = playerName(static_cast<PlayerId>(p));
        if (!gains.empty()) event.details.push_back(name + " gained " + gains);
        else if (event.diceTotal) event.details.push_back(name + " gained no resources");
        if (!losses.empty()) event.details.push_back(name + " lost " + losses);
        if (event.diceTotal && after.turn.pendingDiscards[p])
            event.details.push_back(name + " must discard " + std::to_string(after.turn.pendingDiscards[p]) + " cards");
    }
    for (const auto& message : engineEvents) {
        if (message.starts_with("bank shortage:") || message == "no resource can be stolen") event.details.push_back(message);
    }
    if (after.awards.longestRoad && after.awards.longestRoad != before.awards.longestRoad)
        event.details.push_back(playerName(*after.awards.longestRoad) + " claimed longest road");
    if (after.awards.largestArmy && after.awards.largestArmy != before.awards.largestArmy)
        event.details.push_back(playerName(*after.awards.largestArmy) + " claimed largest army");
    if (after.winner && after.winner != before.winner && !std::holds_alternative<ObserveVictory>(action))
        event.details.push_back(playerName(*after.winner) + " won the game");
    return event;
}

bool writeStateJson(const GameState& state, const std::filesystem::path& output, std::string* error) {
    return writeStateJsonImpl(state, nullptr, nullptr, output, error);
}

bool writeStateJson(const GameState& state, const MctsResult& analysis,
                    const std::filesystem::path& output, std::string* error) {
    return writeStateJsonImpl(state, &analysis, nullptr, output, error);
}

bool renderStateImage(const GameState& state, const std::filesystem::path& output, std::string* error) {
    return renderStateImageImpl(state, nullptr, output, error);
}

bool renderStateImage(const GameState& state, const MctsResult& analysis,
                      const std::filesystem::path& output, std::string* error) {
    return renderStateImageImpl(state, &analysis, output, error);
}

bool writeStateJson(const InformationState& state, const std::filesystem::path& output, std::string* error) {
    return writeStateJsonImpl(state.state(), nullptr, nullptr, output, error, &state);
}
bool writeStateJson(const InformationState& state, const MctsResult& analysis,
                    const std::filesystem::path& output, std::string* error) {
    return writeStateJsonImpl(state.state(), &analysis, nullptr, output, error, &state);
}
bool renderStateImage(const InformationState& state, const std::filesystem::path& output, std::string* error) {
    return renderStateImageImpl(state.state(), nullptr, output, error, &state);
}
bool renderStateImage(const InformationState& state, const MctsResult& analysis,
                      const std::filesystem::path& output, std::string* error) {
    return renderStateImageImpl(state.state(), &analysis, output, error, &state);
}

struct LiveVisualizer::Impl {
    std::optional<InformationState> information;
    std::filesystem::path snapshot;
    std::filesystem::path readyFile;
    std::filesystem::path commandFile;
    std::filesystem::path layoutFile;
    std::filesystem::path analysisFile;
    std::filesystem::path catalogFile;
    std::filesystem::path replyFile;
    mutable std::mutex mutex;
    std::optional<GameState> publishedState;
    std::uint64_t positionVersion{};
    std::uint64_t publishedVersion{};
    bool dirty{true};
    std::vector<Action> legalActions;
    std::unordered_map<std::string, Action> moves;
    std::vector<Action> extraMoves;
    std::vector<Action> analysisActions;
    std::vector<std::string> analysisIds;
    std::vector<ViewerEvent> events;
    bool canRewind{};
    std::array<bool, PlayerCount> treeRunning{true, true};
    std::optional<PlayerId> discardPlayer;
    std::array<bool, PlayerCount> discardSelected{};
#if !defined(_WIN32)
    mutable pid_t process{-1};
#endif

    bool publish(const GameState& state, const MctsResult* analysis, std::string* error) {
        const bool stateChanged = !publishedState || *publishedState != state;
        if (stateChanged) { ++positionVersion; dirty = true; }
        const bool positionChanged = publishedVersion != positionVersion;
        if (positionChanged) {
            const auto* view = information ? &*information : nullptr;
            legalActions = view ? view->legalTransformations() : Game(state).legalTransformations();
            if (state.turn.phase == Phase::Discarding) {
                const auto actor = discardPlayer.value_or(state.turn.decisionPlayer);
                legalActions = view ? view->discardChoices(actor) : Game(state).discardChoices(actor);
            }
            // Keep legacy CLI indices, exposing resign as an additional viewer move.
            const bool canResign = std::any_of(legalActions.begin(), legalActions.end(),
                [](const Action& action) { return std::holds_alternative<Resign>(action); });
            std::erase_if(legalActions, [](const Action& action) { return std::holds_alternative<Resign>(action); });
            if (canResign) legalActions.emplace_back(Resign{});
            moves.clear();
            for (const auto& action : legalActions) moves.emplace(moveId(action), action);
            extraMoves.clear();
            analysisActions.clear();
            analysisIds.clear();
        }
        bool catalogChanged = positionChanged;
        const bool sameAnalysisActions = analysis && analysisActions.size() == analysis->actions.size() &&
            std::equal(analysisActions.begin(), analysisActions.end(), analysis->actions.begin(),
                [](const Action& action, const auto& evaluation) { return action == evaluation.action; });
        if (analysis && !sameAnalysisActions) {
            analysisActions.clear();
            analysisIds.clear();
            for (const auto& evaluation : analysis->actions) {
                analysisActions.push_back(evaluation.action);
                analysisIds.push_back(moveId(evaluation.action));
                if (moves.try_emplace(analysisIds.back(), evaluation.action).second) {
                    extraMoves.push_back(evaluation.action);
                    catalogChanged = true;
                }
            }
        }
        if (catalogChanged && !publishJson(catalogFile, [&](std::ostream& out) {
            out << "{\"position_version\":" << positionVersion << ",\"moves\":[";
            const auto actor = discardPlayer.value_or(state.turn.decisionPlayer);
            for (std::size_t i = 0; i < extraMoves.size(); ++i) {
                if (i) out << ',';
                out << '{'; writeActionFields(out, extraMoves[i], actor); out << '}';
            }
            out << "]}";
        }, error)) return false;
        if (dirty) {
            const LiveControls controls{positionVersion, canRewind, treeRunning, discardPlayer, discardSelected, &events};
            const auto next = std::filesystem::path(snapshot.string() + ".next");
            if (!writeStateJsonImpl(state, nullptr, &controls, next, error,
                                   information ? &*information : nullptr, false, &legalActions)) return false;
            std::error_code filesystemError;
            std::filesystem::rename(next, snapshot, filesystemError);
            if (filesystemError) { if (error) *error = filesystemError.message(); return false; }
            publishedState = state;
            publishedVersion = positionVersion;
            dirty = false;
        }
        // Frequent updates contain scores only. No legal-action generation, hidden
        // information summaries, geometry serialization, or board rendering here.
        return publishJson(analysisFile, [&](std::ostream& out) {
            out << "{\"position_version\":" << positionVersion << ",\"mcts\":";
            writeAnalysis(out, analysis, true, &analysisIds);
            out << '}';
        }, error);
    }

    ~Impl() {
#if !defined(_WIN32)
        if (process > 0) {
            ::kill(process, SIGTERM);
            ::waitpid(process, nullptr, 0);
        }
#endif
        std::error_code ignored;
        std::filesystem::remove(snapshot, ignored);
        std::filesystem::remove(readyFile, ignored);
        std::filesystem::remove(commandFile, ignored);
        std::filesystem::remove(commandFile.string() + ".next", ignored);
        std::filesystem::remove(commandFile.string() + ".reading", ignored);
        std::filesystem::remove(snapshot.string() + ".next", ignored);
        for (const auto& path : {layoutFile, analysisFile, catalogFile, replyFile}) {
            std::filesystem::remove(path, ignored);
            std::filesystem::remove(path.string() + ".next", ignored);
        }
    }
};

LiveVisualizer::LiveVisualizer() : impl_(std::make_unique<Impl>()) {}
LiveVisualizer::~LiveVisualizer() = default;
LiveVisualizer::LiveVisualizer(LiveVisualizer&&) noexcept = default;
LiveVisualizer& LiveVisualizer::operator=(LiveVisualizer&&) noexcept = default;

bool LiveVisualizer::running() const {
#if defined(_WIN32)
    return false;
#else
    if (!impl_ || impl_->process <= 0) return false;
    int status = 0;
    if (::waitpid(impl_->process, &status, WNOHANG) == impl_->process) {
        impl_->process = -1;
        return false;
    }
    return true;
#endif
}

bool LiveVisualizer::update(const GameState& state, std::string* error) {
    if (!running()) {
        if (error) *error = "live visualizer is not running";
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->publish(state, nullptr, error);
}

bool LiveVisualizer::update(const GameState& state, const MctsResult& analysis, std::string* error) {
    if (!running()) {
        if (error) *error = "live visualizer is not running";
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->publish(state, &analysis, error);
}

void LiveVisualizer::setRewindAvailable(bool available) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->canRewind != available) impl_->dirty = true;
    impl_->canRewind = available;
}

void LiveVisualizer::setTreeRunning(PlayerId player, bool running) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->treeRunning[index(player)] != running) impl_->dirty = true;
    impl_->treeRunning[index(player)] = running;
}

void LiveVisualizer::setInformationState(const InformationState& state) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->information || *impl_->information != state) {
        ++impl_->positionVersion;
        impl_->dirty = true;
    }
    impl_->information = state;
}

void LiveVisualizer::setDiscardInput(std::optional<PlayerId> player, std::array<bool, PlayerCount> selected) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->discardPlayer != player || impl_->discardSelected != selected) {
        ++impl_->positionVersion;
        impl_->dirty = true;
    }
    impl_->discardPlayer = player;
    impl_->discardSelected = selected;
}

void LiveVisualizer::setEvents(const std::vector<ViewerEvent>& events) {
    std::lock_guard lock(impl_->mutex);
    impl_->events = events;
    impl_->dirty = true;
}

std::uint64_t LiveVisualizer::positionVersion() const {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->positionVersion;
}

std::optional<Action> LiveVisualizer::resolveMove(const std::string& id, std::uint64_t version) const {
    std::lock_guard lock(impl_->mutex);
    if (version != impl_->positionVersion) return std::nullopt;
    const auto found = impl_->moves.find(id);
    return found == impl_->moves.end() ? std::nullopt : std::optional(found->second);
}

void LiveVisualizer::acknowledge(const ViewerCommand& command, bool accepted, const std::string& message) {
    if (!command.requestId) return;
    std::lock_guard lock(impl_->mutex);
    publishJson(impl_->replyFile, [&](std::ostream& out) {
        out << "{\"request_id\":" << command.requestId
            << ",\"accepted\":" << (accepted ? "true" : "false")
            << ",\"position_version\":" << impl_->positionVersion << ",\"message\":";
        writeJsonString(out, message); out << '}';
    });
}

std::optional<ViewerCommand> LiveVisualizer::takeCommand(std::string* error) {
    if (!impl_ || impl_->commandFile.empty()) return std::nullopt;
    const auto reading = std::filesystem::path(impl_->commandFile.string() + ".reading");
    std::error_code filesystemError;
    std::filesystem::rename(impl_->commandFile, reading, filesystemError);
    if (filesystemError) {
        if (filesystemError == std::errc::no_such_file_or_directory) return std::nullopt;
        if (error) *error = "could not claim viewer command: " + filesystemError.message();
        return std::nullopt;
    }

    std::ifstream input(reading);
    std::string type;
    ViewerCommand command;
    bool valid = static_cast<bool>(input >> type >> command.positionVersion);
    if (valid && type == "move") {
        command.type = ViewerCommandType::SelectMove;
        valid = static_cast<bool>(input >> std::quoted(command.moveId));
    } else if (valid && (type == "action" || type == "transform")) {
        command.type = type == "action" ? ViewerCommandType::SelectAction : ViewerCommandType::SelectTransformation;
        valid = static_cast<bool>(input >> command.actionIndex);
    } else if (valid && type == "winner") {
        std::size_t player = 0;
        valid = static_cast<bool>(input >> player) && player < PlayerCount;
        command.type = ViewerCommandType::ObserveVictory;
        command.player = static_cast<PlayerId>(player);
    } else if (valid && type == "random") {
        command.type = ViewerCommandType::RandomChance;
    } else if (valid && type == "rewind") {
        command.type = ViewerCommandType::Rewind;
    } else if (valid && type == "tree") {
        std::size_t player = 0;
        unsigned enabled = 0;
        valid = static_cast<bool>(input >> player >> enabled) &&
                player < PlayerCount && enabled <= 1;
        if (valid) {
            command.type = ViewerCommandType::ToggleTree;
            command.player = static_cast<PlayerId>(player);
            command.enabled = enabled != 0;
        }
    } else {
        valid = false;
    }
    std::string trailing;
    if (valid && (input >> trailing)) {
        valid = trailing == "request" && static_cast<bool>(input >> command.requestId) && command.requestId > 0;
        if (valid && (input >> trailing)) valid = false;
    }
    std::filesystem::remove(reading, filesystemError);
    if (!valid) {
        if (error) *error = "viewer sent a malformed command";
        return std::nullopt;
    }
    return command;
}

bool LiveVisualizer::start(const GameState& state, std::string* error) {
    if (running()) {
        if (error) *error = "live visualizer is already running";
        return false;
    }
#if defined(_WIN32)
    if (error) *error = "live visualizer process launching is not implemented on Windows";
    return false;
#else
    const auto identifier = std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    impl_->snapshot = std::filesystem::temp_directory_path() / ("catan-live-" + identifier + ".json");
    impl_->readyFile = std::filesystem::temp_directory_path() / ("catan-live-" + identifier + ".ready");
    impl_->commandFile = std::filesystem::temp_directory_path() / ("catan-live-" + identifier + ".command");
    impl_->layoutFile = impl_->snapshot.string() + ".layout";
    impl_->analysisFile = impl_->snapshot.string() + ".analysis";
    impl_->catalogFile = impl_->snapshot.string() + ".catalog";
    impl_->replyFile = impl_->commandFile.string() + ".reply";
    {
        std::lock_guard lock(impl_->mutex);
        impl_->publishedState.reset();
        impl_->positionVersion = 0;
        impl_->publishedVersion = 0;
        impl_->dirty = true;
        impl_->canRewind = false;
        impl_->treeRunning = {true, true};
        if (!publishJson(impl_->layoutFile, [&](std::ostream& out) {
            out << '{'; writeTopology(out); writeBoardLayout(out, state, false); out << '}';
        }, error) || !impl_->publish(state, nullptr, error)) return false;
    }

    std::vector<std::string> arguments{
        CATAN_PYTHON_EXECUTABLE,
        CATAN_RENDER_SCRIPT,
        "--live",
        "--input", impl_->snapshot.string(),
        "--parent-pid", std::to_string(::getpid()),
        "--ready-file", impl_->readyFile.string(),
        "--command-file", impl_->commandFile.string()
    };
    std::vector<char*> argumentPointers;
    argumentPointers.reserve(arguments.size() + 1);
    for (auto& argument : arguments) argumentPointers.push_back(argument.data());
    argumentPointers.push_back(nullptr);

    const int spawnStatus = ::posix_spawnp(&impl_->process, CATAN_PYTHON_EXECUTABLE, nullptr, nullptr,
                                            argumentPointers.data(), environ);
    if (spawnStatus != 0) {
        impl_->process = -1;
        if (error) *error = "could not start Python visualizer (error " + std::to_string(spawnStatus) + ')';
        return false;
    }

    for (unsigned attempt = 0; attempt < 120; ++attempt) {
        if (std::filesystem::exists(impl_->readyFile)) return true;
        int childStatus = 0;
        if (::waitpid(impl_->process, &childStatus, WNOHANG) == impl_->process) {
            impl_->process = -1;
            if (error) *error = "Python visualizer exited during startup";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ::kill(impl_->process, SIGTERM);
    ::waitpid(impl_->process, nullptr, 0);
    impl_->process = -1;
    if (error) *error = "timed out waiting for the Python visualizer";
    return false;
#endif
}

} // namespace catan::debug
