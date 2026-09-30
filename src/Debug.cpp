#include "catan/Debug.h"

#include <cstdlib>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <thread>
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

bool writeStateJsonImpl(const GameState& state, const MctsResult* analysis, const LiveControls* controls,
                        const std::filesystem::path& output, std::string* error) {
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

    const auto& topology = BoardTopology::standard();
    out << "{\n\"schema\":\"catan-debug-state-v1\",\n\"controls\":";
    if (controls) {
        out << "{\"position_version\":" << controls->positionVersion
            << ",\"can_rewind\":" << (controls->canRewind ? "true" : "false")
            << ",\"tree_running\":["
            << (controls->treeRunning[0] ? "true" : "false") << ','
            << (controls->treeRunning[1] ? "true" : "false") << "]},\n";
    } else {
        out << "null,\n";
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

    out << "\"board\":{\"hexes\":[";
    for (std::size_t h = 0; h < state.board.hexes.size(); ++h) {
        if (h) out << ',';
        const auto& hex = state.board.hexes[h];
        out << "{\"id\":" << h << ",\"terrain\":\"" << toString(hex.terrain)
            << "\",\"number\":" << +hex.number
            << ",\"robber\":" << (state.board.robberHex.value == h ? "true" : "false") << '}';
    }
    out << "],\"ports\":[";
    for (std::size_t p = 0; p < state.board.ports.size(); ++p) {
        if (p) out << ',';
        const auto& port = state.board.ports[p];
        out << "{\"edge\":" << +port.edge.value << ",\"type\":\"" << toString(port.type) << "\"}";
    }
    out << "],\"buildings\":[";
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
        writeDevelopmentCards(out, value.developmentCards.cards);
        out << ",\"bought_this_turn\":";
        writeDevelopmentCards(out, value.developmentCards.boughtThisTurn);
        out << ",\"played_knights\":" << +value.playedKnights
            << ",\"public_vp\":" << +publicVictoryPoints(state, player)
            << ",\"total_vp\":" << +victoryPoints(state, player) << '}';
    }
    out << "],\n\"bank\":";
    writeResources(out, state.bank.cards);
    out << ",\n\"development_deck\":";
    writeDevelopmentCards(out, state.developmentDeck.cards);
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
    out << "},\n\"mcts\":";
    if (!analysis) {
        out << "null";
    } else {
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
        if (bestIndex) out << *bestIndex;
        else out << "null";
        out << ",\"actions\":[" << std::setprecision(17);
        for (std::size_t i = 0; i < analysis->actions.size(); ++i) {
            if (i) out << ',';
            const auto& evaluation = analysis->actions[i];
            out << "{\"index\":" << i << ",\"description\":";
            writeJsonString(out, toString(evaluation.action));
            out << ",\"visits\":" << evaluation.visits
                << ",\"win_probability\":" << evaluation.estimatedWinProbability
                << ",\"chance_probability\":";
            if (evaluation.chanceProbability) out << *evaluation.chanceProbability;
            else out << "null";
            out << ",\"recommended\":" << (bestIndex && *bestIndex == i ? "true" : "false")
                << ",\"highlight_targets\":";
            writeHighlightTargets(out, evaluation.action, analysis->perspective);
            out << '}';
        }
        out << "]}";
    }
    out << "\n}\n";

    if (!out) {
        if (error) *error = "failed while writing " + output.string();
        return false;
    }
    return true;
}

bool renderStateImageImpl(const GameState& state, const MctsResult* analysis,
                          const std::filesystem::path& output, std::string* error) {
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
    const bool wroteSnapshot = analysis
        ? writeStateJsonImpl(state, analysis, nullptr, snapshot, error)
        : writeStateJsonImpl(state, nullptr, nullptr, snapshot, error);
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

struct LiveVisualizer::Impl {
    std::filesystem::path snapshot;
    std::filesystem::path readyFile;
    std::filesystem::path commandFile;
    mutable std::mutex mutex;
    std::optional<GameState> publishedState;
    std::uint64_t positionVersion{};
    bool canRewind{};
    std::array<bool, PlayerCount> treeRunning{true, true};
#if !defined(_WIN32)
    mutable pid_t process{-1};
#endif

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
    if (!impl_->publishedState || *impl_->publishedState != state) {
        impl_->publishedState = state;
        ++impl_->positionVersion;
    }
    const auto next = std::filesystem::path(impl_->snapshot.string() + ".next");
    const LiveControls controls{impl_->positionVersion, impl_->canRewind, impl_->treeRunning};
    if (!writeStateJsonImpl(state, nullptr, &controls, next, error)) return false;
    std::error_code renameError;
    std::filesystem::rename(next, impl_->snapshot, renameError);
    if (renameError) {
        if (error) *error = "could not publish visualizer state: " + renameError.message();
        return false;
    }
    return true;
}

bool LiveVisualizer::update(const GameState& state, const MctsResult& analysis, std::string* error) {
    if (!running()) {
        if (error) *error = "live visualizer is not running";
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->publishedState || *impl_->publishedState != state) {
        impl_->publishedState = state;
        ++impl_->positionVersion;
    }
    const auto next = std::filesystem::path(impl_->snapshot.string() + ".next");
    const LiveControls controls{impl_->positionVersion, impl_->canRewind, impl_->treeRunning};
    if (!writeStateJsonImpl(state, &analysis, &controls, next, error)) return false;
    std::error_code renameError;
    std::filesystem::rename(next, impl_->snapshot, renameError);
    if (renameError) {
        if (error) *error = "could not publish visualizer state: " + renameError.message();
        return false;
    }
    return true;
}

void LiveVisualizer::setRewindAvailable(bool available) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    impl_->canRewind = available;
}

void LiveVisualizer::setTreeRunning(PlayerId player, bool running) {
    if (!impl_) return;
    std::lock_guard lock(impl_->mutex);
    impl_->treeRunning[index(player)] = running;
}

std::uint64_t LiveVisualizer::positionVersion() const {
    if (!impl_) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->positionVersion;
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
    if (valid && type == "action") {
        command.type = ViewerCommandType::SelectAction;
        valid = static_cast<bool>(input >> command.actionIndex);
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
    if (valid && (input >> trailing)) valid = false;
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
    {
        std::lock_guard lock(impl_->mutex);
        impl_->publishedState = state;
        impl_->positionVersion = 1;
        impl_->canRewind = false;
        impl_->treeRunning = {true, true};
        const LiveControls controls{impl_->positionVersion, impl_->canRewind, impl_->treeRunning};
        if (!writeStateJsonImpl(state, nullptr, &controls, impl_->snapshot, error)) return false;
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
