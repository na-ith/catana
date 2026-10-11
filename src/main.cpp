#include "catan/Debug.h"
#include "catan/BoardImport.h"
#include "catan/Game.h"
#include "catan/Mcts.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <sys/select.h>
#include <unistd.h>
#endif

namespace {

void printResources(const catan::Resources& resources) {
    std::cout << '[';
    for (std::size_t i = 0; i < catan::ResourceCount; ++i) {
        if (i) std::cout << ' ';
        std::cout << catan::toString(static_cast<catan::Resource>(i)) << '=' << +resources[i];
    }
    std::cout << ']';
}

void printState(const catan::GameState& state, std::optional<catan::PlayerId> observer = {}) {
    std::cout << "\nTurn " << state.turnNumber << " | " << catan::toString(state.turn.phase)
              << " | acting: " << catan::toString(state.turn.decisionPlayer) << '\n';
    for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
        auto player = static_cast<catan::PlayerId>(p);
        std::cout << "  " << catan::toString(player) << ": ";
        if (observer && player != *observer) std::cout << "unknown total VP";
        else std::cout << +catan::victoryPoints(state, player) << " VP";
        std::cout << " (" << +catan::publicVictoryPoints(state, player) << " public), resources ";
        printResources(state.players[p].resources);
        std::cout << ", knights=" << +state.players[p].playedKnights << '\n';
    }
    std::cout << "  Hexes:";
    for (std::size_t h = 0; h < catan::BoardTopology::HexCount; ++h) {
        const auto& hex = state.board.hexes[h];
        std::cout << " H" << h << '=' << catan::toString(hex.terrain);
        if (hex.number) std::cout << '/' << +hex.number;
        if (state.board.robberHex.value == h) std::cout << "(robber)";
    }
    std::cout << "\n  Buildings:";
    for (std::size_t v = 0; v < catan::BoardTopology::VertexCount; ++v) {
        const auto& b = state.board.buildings[v];
        if (b) std::cout << " V" << v << '=' << (b->owner == catan::PlayerId::One ? "P1" : "P2")
                         << (b->type == catan::BuildingType::City ? 'C' : 'S');
    }
    std::cout << "\n  Roads:";
    for (std::size_t e = 0; e < catan::BoardTopology::EdgeCount; ++e) {
        const auto& road = state.board.roads[e];
        if (road) std::cout << " E" << e << '=' << (road->owner == catan::PlayerId::One ? "P1" : "P2");
    }
    std::cout << '\n';
}

std::string describeAction(const catan::Action& action, const catan::GameState& state) {
    std::ostringstream out;
    out << catan::toString(action);
    const auto& topology = catan::BoardTopology::standard();
    auto describeVertex = [&](catan::VertexId vertex) {
        out << " (";
        bool first = true;
        for (auto hex : topology.vertices[vertex.value].hexes) {
            if (!hex) continue;
            if (!first) out << ", ";
            const auto& tile = state.board.hexes[hex->value];
            out << 'H' << +hex->value << ':' << catan::toString(tile.terrain);
            if (tile.number) out << '/' << +tile.number;
            first = false;
        }
        for (const auto& port : state.board.ports) {
            const auto ends = topology.edges[port.edge.value].vertices;
            if (ends[0] != vertex && ends[1] != vertex) continue;
            if (!first) out << ", ";
            out << "port:" << catan::toString(port.type);
            first = false;
        }
        out << ')';
    };
    if (const auto* value = std::get_if<catan::PlaceSettlement>(&action)) describeVertex(value->vertex);
    else if (const auto* value = std::get_if<catan::BuildSettlement>(&action)) describeVertex(value->vertex);
    else if (const auto* value = std::get_if<catan::BuildCity>(&action)) describeVertex(value->vertex);
    else if (const auto* value = std::get_if<catan::PlaceRoad>(&action)) {
        const auto vertices = topology.edges[value->edge.value].vertices;
        out << " (V" << +vertices[0].value << "-V" << +vertices[1].value << ')';
    } else if (const auto* value = std::get_if<catan::BuildRoad>(&action)) {
        const auto vertices = topology.edges[value->edge.value].vertices;
        out << " (V" << +vertices[0].value << "-V" << +vertices[1].value << ')';
    }
    return out.str();
}

template <typename Random>
std::size_t sampleChanceOutcome(const std::vector<catan::WeightedAction>& outcomes, Random& random) {
    double total = 0.0;
    for (const auto& outcome : outcomes) total += outcome.probability;
    std::uniform_real_distribution<double> pick(0.0, total);
    double target = pick(random);
    for (std::size_t i = 0; i < outcomes.size(); ++i) {
        target -= outcomes[i].probability;
        if (target <= 0.0) return i;
    }
    return outcomes.size() - 1;
}

bool waitForInputOrViewer(catan::debug::LiveVisualizer& visualizer, bool viewerEnabled,
                          std::string& line,
                          std::optional<catan::debug::ViewerCommand>& viewerCommand) {
    if (!viewerEnabled) return static_cast<bool>(std::getline(std::cin, line));
#if defined(_WIN32)
    return static_cast<bool>(std::getline(std::cin, line));
#else
    while (true) {
        std::string commandError;
        if (auto command = visualizer.takeCommand(&commandError)) {
            viewerCommand = *command;
            return true;
        }
        if (!commandError.empty()) std::cerr << "Viewer command error: " << commandError << '\n';

        fd_set inputs;
        FD_ZERO(&inputs);
        FD_SET(STDIN_FILENO, &inputs);
        timeval timeout{0, 50'000};
        const int ready = ::select(STDIN_FILENO + 1, &inputs, nullptr, nullptr, &timeout);
        if (ready > 0 && FD_ISSET(STDIN_FILENO, &inputs))
            return static_cast<bool>(std::getline(std::cin, line));
        if (ready < 0) return false;
    }
#endif
}

struct HistoryEntry {
    std::optional<catan::GameState> state;
    std::mt19937 random;
    std::array<std::optional<catan::InformationState>, catan::PlayerCount> information;
    std::vector<catan::debug::ViewerEvent> viewerEvents;
};

// Every viewer request gets an engine acknowledgement, including early returns.
struct ViewerReceipt {
    catan::debug::LiveVisualizer& viewer;
    const std::optional<catan::debug::ViewerCommand>& command;
    bool accepted{};
    std::string message{"Move could not be applied."};
    ~ViewerReceipt() {
        if (command) viewer.acknowledge(*command, accepted, message);
    }
};

class LiveMctsAnalysis {
public:
    LiveMctsAnalysis(catan::debug::LiveVisualizer& visualizer, std::size_t workerCount,
                     std::optional<catan::PlayerId> perspective = {})
        : visualizer_(visualizer), workerCount_(std::max<std::size_t>(1, workerCount)),
          perspective_(perspective) {
        if (perspective_) trees_[catan::index(catan::opponent(*perspective_))].enabled = false;
    }
    ~LiveMctsAnalysis() { stop(); }

    LiveMctsAnalysis(const LiveMctsAnalysis&) = delete;
    LiveMctsAnalysis& operator=(const LiveMctsAnalysis&) = delete;

    void startNew(const catan::GameState& state,
                  const std::array<std::optional<catan::InformationState>, catan::PlayerCount>& information,
                  std::uint32_t seed) {
        stop();
        publicState_ = state;
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
            auto& tree = trees_[p];
            tree.latest.reset();
            tree.workerResults.assign(workerCount_, std::nullopt);
            tree.sessions.clear();
            if (!information[p]) {
                tree.enabled = false;
                visualizer_.setTreeRunning(static_cast<catan::PlayerId>(p), false);
                continue;
            }
            tree.sessions.reserve(workerCount_);
            for (std::size_t worker = 0; worker < workerCount_; ++worker)
                tree.sessions.push_back(makeSession(
                    *information[p], seed, worker));
            visualizer_.setTreeRunning(static_cast<catan::PlayerId>(p), tree.enabled);
        }
        publishCurrent();
        resume();
    }

    void resume() {
        stop();
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) resumeTree(p);
    }

    bool advance(const catan::Action& action, const catan::GameState& state,
                 const std::array<std::optional<catan::InformationState>, catan::PlayerCount>& information) {
        stop();
        bool reused = false;
        publicState_ = state;
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
            auto& tree = trees_[p];
            if (!information[p]) continue;
            std::vector<catan::MctsResult> results;
            results.reserve(workerCount_);
            for (std::size_t worker = 0; worker < tree.sessions.size(); ++worker) {
                auto& session = tree.sessions[worker];
                catan::Action observedAction = action;
                if (std::holds_alternative<catan::DrawDevelopmentCard>(action) &&
                    session->state().decisionPlayer() != static_cast<catan::PlayerId>(p))
                    observedAction = catan::DrawUnknownDevelopmentCard{};
                reused |= session->synchronize(observedAction, *information[p]);
                results.push_back(session->result());
            }
            tree.workerResults.clear();
            for (const auto& result : results) tree.workerResults.push_back(result);
            tree.latest = mergeCompatible(results);
        }
        publishCurrent();
        resume();
        return reused;
    }

    void stop() {
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) stopTree(p);
    }

    std::optional<catan::MctsResult> latest() const {
        std::lock_guard lock(mutex_);
        return trees_[catan::index(inputPlayer_.value_or(publicState_.turn.decisionPlayer))].latest;
    }

    void setInputPlayer(std::optional<catan::PlayerId> player) {
        {
            std::lock_guard lock(mutex_);
            inputPlayer_ = player;
        }
        publishCurrent();
    }

    void setEnabled(catan::PlayerId player, bool enabled) {
        const auto p = catan::index(player);
        if (perspective_ && player != *perspective_) return;
        stopTree(p);
        {
            std::lock_guard lock(mutex_);
            trees_[p].enabled = enabled;
        }
        visualizer_.setTreeRunning(player, enabled);
        if (enabled) resumeTree(p);
        publishCurrent();
    }

private:
    struct PlayerTree {
        bool enabled{true};
        std::optional<catan::MctsResult> latest;
        std::vector<std::optional<catan::MctsResult>> workerResults;
        std::vector<std::unique_ptr<catan::InformationMctsSession>> sessions;
        std::vector<std::jthread> workers;
        std::chrono::steady_clock::time_point lastPublished{};
    };

    std::unique_ptr<catan::InformationMctsSession> makeSession(
        const catan::InformationState& information, std::uint32_t seed, std::size_t worker) const {
        catan::MctsConfig config;
        config.iterations = std::numeric_limits<std::size_t>::max();
        config.progressInterval = 100;
        config.seed = seed + static_cast<std::uint32_t>(worker * 0x9e3779b9U) +
                      static_cast<std::uint32_t>((catan::index(information.observer()) + 1) * 0x85ebca6bU);
        return std::make_unique<catan::InformationMctsSession>(information, config);
    }

    static std::optional<catan::MctsResult> mergeCompatible(
        const std::vector<catan::MctsResult>& results) {
        if (results.empty()) return std::nullopt;
        catan::MctsResult merged = results.front();
        merged.actions.clear();
        merged.iterations = 0;
        merged.workerCount = results.size();
        merged.maximumDepth = 0;
        merged.bestAction.reset();
        for (const auto& result : results) {
            if (result.perspective != merged.perspective || result.rootPlayer != merged.rootPlayer ||
                result.chanceNode != merged.chanceNode) return std::nullopt;
            merged.iterations += result.iterations;
            merged.maximumDepth = std::max(merged.maximumDepth, result.maximumDepth);
            for (const auto& value : result.actions) {
                auto found = std::find_if(merged.actions.begin(), merged.actions.end(),
                    [&](const auto& item) { return item.action == value.action; });
                if (found == merged.actions.end()) merged.actions.push_back(value);
                else {
                    const auto visits = found->visits + value.visits;
                    if (visits) found->estimatedWinProbability =
                        (found->estimatedWinProbability * found->visits + value.estimatedWinProbability * value.visits) / visits;
                    found->visits = visits;
                }
            }
        }
        if (!merged.chanceNode) {
            const catan::ActionEvaluation* best = nullptr;
            for (const auto& value : merged.actions) {
                if (!value.visits) continue;
                if (!best || (merged.rootPlayer == merged.perspective
                    ? value.estimatedWinProbability > best->estimatedWinProbability
                    : value.estimatedWinProbability < best->estimatedWinProbability)) best = &value;
            }
            if (best) merged.bestAction = best->action;
        }
        return merged;
    }

    void stopTree(std::size_t player) {
        auto& workers = trees_[player].workers;
        for (auto& worker : workers) worker.request_stop();
        for (auto& worker : workers) if (worker.joinable()) worker.join();
        workers.clear();
    }

    void resumeTree(std::size_t player) {
        auto& tree = trees_[player];
        if (!tree.enabled || tree.sessions.empty() ||
            publicState_.turn.phase == catan::Phase::Finished || !visualizer_.running()) return;
        tree.workers.reserve(tree.sessions.size());
        for (std::size_t worker = 0; worker < tree.sessions.size(); ++worker) {
            tree.workers.emplace_back([this, player, worker](std::stop_token stopToken) {
                trees_[player].sessions[worker]->run(
                    stopToken, [this, player, worker](const catan::MctsResult& progress) {
                        publish(player, worker, progress);
                    });
            });
        }
    }

    void publish(std::size_t player, std::size_t worker, const catan::MctsResult& progress) {
        std::optional<catan::MctsResult> combined;
        std::size_t generation = 0;
        catan::GameState state;
        {
            std::lock_guard lock(mutex_);
            auto& tree = trees_[player];
            tree.workerResults[worker] = progress;
            const auto now = std::chrono::steady_clock::now();
            if (!tree.latest || now - tree.lastPublished >= std::chrono::milliseconds(150)) {
                std::vector<catan::MctsResult> available;
                for (const auto& result : tree.workerResults) if (result) available.push_back(*result);
                combined = mergeCompatible(available);
                tree.latest = combined;
                tree.lastPublished = now;
                if (combined && player == catan::index(inputPlayer_.value_or(publicState_.turn.decisionPlayer))) {
                    state = publicState_;
                    generation = ++publishGeneration_;
                } else {
                    combined.reset();
                }
            }
        }
        if (combined) {
            std::lock_guard publishLock(publishMutex_);
            {
                std::lock_guard lock(mutex_);
                if (generation != publishGeneration_) return;
            }
            std::string ignored;
            visualizer_.update(state, *combined, &ignored);
        }
    }

    void publishCurrent() {
        std::lock_guard publishLock(publishMutex_);
        std::optional<catan::MctsResult> analysis;
        catan::GameState state;
        {
            std::lock_guard lock(mutex_);
            state = publicState_;
            analysis = trees_[catan::index(inputPlayer_.value_or(state.turn.decisionPlayer))].latest;
            ++publishGeneration_;
        }
        std::string ignored;
        if (analysis) visualizer_.update(state, *analysis, &ignored);
        else visualizer_.update(state, &ignored);
    }

    catan::debug::LiveVisualizer& visualizer_;
    const std::size_t workerCount_;
    mutable std::mutex mutex_;
    std::optional<catan::PlayerId> inputPlayer_;
    std::mutex publishMutex_;
    std::array<PlayerTree, catan::PlayerCount> trees_;
    const std::optional<catan::PlayerId> perspective_;
    catan::GameState publicState_;
    std::size_t publishGeneration_{};
};

} // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 1;
    bool seedWasSet = false;
    bool uninitialized = false;
    std::optional<std::filesystem::path> boardFile;
    bool enableVisualizer = false;
    std::optional<catan::PlayerId> perspective;
    std::size_t mctsThreads = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1U, 8U);
    for (int argument = 1; argument < argc; ++argument) {
        const std::string value = argv[argument];
        if (value == "--visualizer") {
            enableVisualizer = true;
        } else if (value == "--perspective") {
            if (++argument == argc || perspective) {
                std::cerr << "--perspective requires p1 or p2\n";
                return 2;
            }
            const std::string player = argv[argument];
            if (player == "p1" || player == "1") perspective = catan::PlayerId::One;
            else if (player == "p2" || player == "2") perspective = catan::PlayerId::Two;
            else { std::cerr << "--perspective requires p1 or p2\n"; return 2; }
        } else if (value == "--uninitialized") {
            uninitialized = true;
        } else if (value == "--board") {
            if (++argument == argc || boardFile) {
                std::cerr << "--board requires one snapshot or capture file\n";
                return 2;
            }
            boardFile = argv[argument];
        } else if (value == "--mcts-threads") {
            if (++argument == argc) {
                std::cerr << "--mcts-threads requires a positive number\n";
                return 2;
            }
            std::istringstream input(argv[argument]);
            if (!(input >> mctsThreads) || !input.eof() || mctsThreads == 0) {
                std::cerr << "invalid MCTS thread count: " << argv[argument] << '\n';
                return 2;
            }
        } else if (value == "--seed") {
            if (++argument == argc) {
                std::cerr << "--seed requires a number\n";
                return 2;
            }
            std::istringstream input(argv[argument]);
            if (!(input >> seed) || !input.eof()) {
                std::cerr << "invalid seed: " << argv[argument] << '\n';
                return 2;
            }
            seedWasSet = true;
        } else if (value == "--help" || value == "-h") {
            std::cout << "usage: catan_cli [SEED | --seed SEED | --uninitialized | --board FILE] "
                         "[--visualizer] [--mcts-threads N] [--perspective p1|p2]\n"
                         "Uninitialized commands: status, init FILE, init-json JSON, q.\n";
            return 0;
        } else if (value.starts_with('-') || seedWasSet) {
            std::cerr << "unknown or duplicate argument: " << value << '\n';
            return 2;
        } else {
            std::istringstream input(value);
            if (!(input >> seed) || !input.eof()) {
                std::cerr << "invalid seed: " << value << '\n';
                return 2;
            }
            seedWasSet = true;
        }
    }
    if (seedWasSet && (uninitialized || boardFile)) {
        std::cerr << "A seed cannot be combined with --uninitialized or --board.\n";
        return 2;
    }
    std::optional<catan::Game> initializedGame;
    if (!uninitialized && !boardFile) initializedGame.emplace(seed);
    if (boardFile) {
        std::string error;
        const auto board = catan::loadColonistBoard(*boardFile, &error);
        if (!board) {
            std::cerr << "Board initialization failed: " << error << '\n';
            return 2;
        }
        initializedGame.emplace(*board);
    }
    if (!initializedGame)
        std::cout << "Colonist stepper is uninitialized. Use 'init FILE' with a decoded "
                     "snapshot JSON or capture JSONL, or 'init-json JSON' for an inline message; "
                     "'status' or 'q' are also available.\n";
    while (!initializedGame) {
        std::cout << "uninitialized> " << std::flush;
        std::string line;
        if (!std::getline(std::cin, line) || line == "q" || line == "quit") return 0;
        if (line == "status") {
            std::cout << "State: uninitialized (no board, legal actions, viewer, or MCTS yet).\n";
            continue;
        }
        const bool inlineJson = line.starts_with("init-json ");
        if (!inlineJson && !line.starts_with("init ")) {
            std::cout << "Initialize first with 'init FILE' or 'init-json JSON'.\n";
            continue;
        }
        const auto first = line.find_first_not_of(' ', inlineJson ? 10 : 5);
        if (first == std::string::npos) {
            std::cout << "init requires a snapshot or capture file.\n";
            continue;
        }
        std::string error;
        const auto board = inlineJson
            ? catan::loadColonistBoardJson(line.substr(first), &error)
            : catan::loadColonistBoard(line.substr(first), &error);
        if (!board) {
            std::cout << "Board initialization failed: " << error << '\n';
            continue;
        }
        initializedGame.emplace(*board);
        boardFile = inlineJson ? "inline JSON" : line.substr(first);
        std::cout << "Board initialized from " << *boardFile << ". Starting initial placement.\n";
    }
    std::array<std::optional<catan::InformationState>, catan::PlayerCount> information;
    for (std::size_t p = 0; p < catan::PlayerCount; ++p)
        if (!perspective || catan::index(*perspective) == p)
            information[p].emplace(initializedGame->state(), static_cast<catan::PlayerId>(p));
    std::optional<catan::Game> game;
    if (!perspective) game = std::move(*initializedGame);
    initializedGame.reset();
    const auto currentState = [&]() -> const catan::GameState& {
        return perspective ? information[catan::index(*perspective)]->state() : game->state();
    };
    catan::debug::LiveVisualizer visualizer;
    if (enableVisualizer) {
        std::string error;
        if (perspective) visualizer.setInformationState(*information[catan::index(*perspective)]);
        if (!visualizer.start(currentState(), &error)) {
            std::cerr << "Could not start live visualizer: " << error << '\n';
            return 2;
        }
    }
    std::mt19937 random(seed ^ 0x9e3779b9U);
    std::vector<HistoryEntry> history;
    std::vector<catan::debug::ViewerEvent> viewerEvents;
    std::uint64_t nextViewerEventId = 1;
    auto recordViewerEvent = [&](const catan::Action& action, const catan::GameState& before,
                                 const std::vector<std::string>& engineEvents) {
        if (!enableVisualizer) return;
        if (std::holds_alternative<catan::RollDice>(action)) viewerEvents.clear();
        auto event = catan::debug::describeViewerEvent(action, before, currentState(), engineEvents);
        event.id = nextViewerEventId++;
        viewerEvents.push_back(std::move(event));
        visualizer.setEvents(viewerEvents);
    };
    // Input staging is deliberately not part of GameState or InformationState.
    std::array<std::optional<catan::Resources>, catan::PlayerCount> pendingDiscardChoices;
    auto inputPlayer = [&]() {
        if (currentState().turn.phase == catan::Phase::Discarding) {
            if (perspective && currentState().turn.pendingDiscards[catan::index(*perspective)] &&
                !pendingDiscardChoices[catan::index(*perspective)]) return *perspective;
            for (std::size_t p = 0; p < catan::PlayerCount; ++p)
                if (currentState().turn.pendingDiscards[p] && !pendingDiscardChoices[p])
                    return static_cast<catan::PlayerId>(p);
        }
        return currentState().turn.decisionPlayer;
    };
    std::optional<catan::MctsResult> currentAnalysis;
    LiveMctsAnalysis liveAnalysis(visualizer, mctsThreads, perspective);
    auto refreshDiscardInput = [&]() {
        if (!enableVisualizer) return;
        std::optional<catan::PlayerId> actor;
        if (currentState().turn.phase == catan::Phase::Discarding) actor = inputPlayer();
        visualizer.setDiscardInput(actor, {pendingDiscardChoices[0].has_value(), pendingDiscardChoices[1].has_value()});
        visualizer.setRewindAvailable(!history.empty() || pendingDiscardChoices[0] || pendingDiscardChoices[1]);
        liveAnalysis.setInputPlayer(actor);
    };
    if (enableVisualizer)
        liveAnalysis.startNew(currentState(), information, seed ^ 0x517cc1b7U);
    std::cout << "Colonist ranked 1v1 stepper (";
    if (boardFile) std::cout << "imported board " << *boardFile;
    else std::cout << "board seed " << seed;
    std::cout << ")\n"
                 "Choose an action number; chance nodes also offer a weighted 'random' option,\n"
                 "'mcts [iterations]' to analyse actions, 'best [iterations]' to play the best action,\n"
                 "'rewind' to undo one action, 'image [file.svg]' to render the state,\n"
                 "'json [file.json]' to export it, or 'q' to quit.\n";
    if (perspective) {
        std::cout << "Perspective: " << catan::toString(*perspective)
                  << ". Opponent development draws remain unknown.\n"
                     "On opponent turns, enter the action they took. Use 'winner p1|p2' to report a victory.\n";
    }
    if (enableVisualizer && !perspective)
        std::cout << "Both player-specific MCTS belief trees are running continuously with "
                  << mctsThreads << " workers per enabled tree.\n";
    else if (enableVisualizer)
        std::cout << "Only " << catan::toString(*perspective) << "'s tree is running with "
                  << mctsThreads << " workers.\n";

    while (true) {
        refreshDiscardInput();
        printState(currentState(), perspective);
        if (currentState().turn.phase == catan::Phase::Discarding)
            std::cout << "Choose discard for " << catan::toString(inputPlayer())
                      << ". Hands stay unchanged until all required choices are recorded.\n";
        if (perspective) {
            const auto& view = *information[catan::index(*perspective)];
            std::cout << "  Opponent development cards: " << view.opponentCardCount()
                      << " unknown; " << view.worldCount() << " possible worlds\n";
            if (inputPlayer() != *perspective)
                std::cout << "Enter the opponent's observed action.\n";
        }
        auto actions = perspective ? information[catan::index(*perspective)]->legalTransformations()
                                   : game->legalTransformations();
        if (currentState().turn.phase == catan::Phase::Discarding)
            actions = perspective ? information[catan::index(*perspective)]->discardChoices(inputPlayer())
                                  : game->discardChoices(inputPlayer());
        auto addEngineBatches = [&](const catan::MctsResult& analysis) {
            for (const auto& evaluation : analysis.actions) {
                const auto* batch = std::get_if<catan::MaritimeTradeBatch>(&evaluation.action);
                if (batch && catan::Game(currentState()).canTradeBatch(*batch) &&
                    std::find(actions.begin(), actions.end(), evaluation.action) == actions.end())
                    actions.push_back(evaluation.action);
            }
        };
        if (currentAnalysis) addEngineBatches(*currentAnalysis);
        const auto chance = perspective ? information[catan::index(*perspective)]->chanceOutcomes()
                                       : game->chanceOutcomes();
        for (std::size_t i = 0; i < actions.size(); ++i) {
            std::cout << std::setw(4) << i << ": " << describeAction(actions[i], currentState());
            for (const auto& outcome : chance)
                if (catan::toString(outcome.action) == catan::toString(actions[i]))
                    std::cout << "  [p=" << std::fixed << std::setprecision(4) << outcome.probability << ']';
            std::cout << '\n';
        }
        if (!chance.empty())
            std::cout << std::setw(4) << actions.size()
                      << ": random  [sample a weighted chance outcome]\n";
        std::cout << "> " << std::flush;
        std::string line;
        std::optional<catan::debug::ViewerCommand> viewerCommand;
        if (!waitForInputOrViewer(visualizer, enableVisualizer, line, viewerCommand)) break;
        ViewerReceipt receipt{visualizer, viewerCommand};
        if (!viewerCommand && (line == "q" || line == "quit")) break;
        if (!viewerCommand && line == "status") {
            std::cout << "State: initialized | " << catan::toString(currentState().turn.phase)
                      << " | acting: " << catan::toString(currentState().turn.decisionPlayer) << '\n';
            continue;
        }
        if (!viewerCommand && (line.starts_with("init ") || line.starts_with("init-json "))) {
            std::cout << "Already initialized; start a new --uninitialized process to import another board.\n";
            continue;
        }

        auto rewindOneAction = [&]() {
            if (pendingDiscardChoices[0] || pendingDiscardChoices[1]) {
                pendingDiscardChoices = {};
                std::erase_if(viewerEvents, [](const auto& event) { return event.type == "discard_selection"; });
                if (enableVisualizer) visualizer.setEvents(viewerEvents);
                refreshDiscardInput();
                std::cout << "Cancelled pending discard selections; game state unchanged.\n";
                return;
            }
            if (history.empty()) {
                std::cout << "Nothing to rewind.\n";
                return;
            }
            liveAnalysis.stop();
            auto previous = std::move(history.back());
            history.pop_back();
            if (previous.state) game.emplace(std::move(*previous.state));
            random = previous.random;
            information = std::move(previous.information);
            viewerEvents = std::move(previous.viewerEvents);
            currentAnalysis.reset();
            if (enableVisualizer) {
                visualizer.setEvents(viewerEvents);
                visualizer.setRewindAvailable(!history.empty());
                std::string error;
                if (perspective) visualizer.setInformationState(*information[catan::index(*perspective)]);
                if (!visualizer.update(currentState(), &error))
                    std::cout << "Visualizer rewind update failed: " << error << '\n';
                liveAnalysis.startNew(
                    currentState(), information,
                    seed ^ 0x517cc1b7U ^ static_cast<std::uint32_t>(history.size()));
            }
            std::cout << "Rewound one action.\n";
        };

        if (viewerCommand) {
            if (viewerCommand->positionVersion != visualizer.positionVersion()) {
                receipt.message = "The position changed. Select a move in the current position.";
                std::cout << "Ignored a stale viewer command.\n";
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::Rewind) {
                receipt.accepted = !history.empty() || pendingDiscardChoices[0] || pendingDiscardChoices[1];
                receipt.message = receipt.accepted ? "Rewound one action." : "There is no action to rewind.";
                rewindOneAction();
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::ToggleTree) {
                if (perspective && viewerCommand->player != *perspective) {
                    receipt.message = "The opponent tree is unavailable in perspective mode.";
                    std::cout << "The opponent tree is unavailable in perspective mode.\n";
                    continue;
                }
                liveAnalysis.setEnabled(viewerCommand->player, viewerCommand->enabled);
                receipt.accepted = true;
                receipt.message = viewerCommand->enabled ? "Analysis resumed." : "Analysis paused.";
                std::cout << catan::toString(viewerCommand->player) << " belief-tree calculation "
                          << (viewerCommand->enabled ? "enabled" : "paused") << ".\n";
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::SelectMove) {
                const auto selected = visualizer.resolveMove(viewerCommand->moveId, viewerCommand->positionVersion);
                if (!selected) {
                    receipt.message = "This move is no longer available.";
                    continue;
                }
                auto found = std::find(actions.begin(), actions.end(), *selected);
                if (found == actions.end()) {
                    const auto* batch = std::get_if<catan::MaritimeTradeBatch>(&*selected);
                    if (!batch || !catan::Game(currentState()).canTradeBatch(*batch)) {
                        receipt.message = "This move is no longer legal.";
                        continue;
                    }
                    actions.push_back(*selected);
                    found = std::prev(actions.end());
                }
                line = std::to_string(static_cast<std::size_t>(found - actions.begin()));
                std::cout << "Viewer selected " << line << ": " << describeAction(*selected, currentState()) << '\n';
            } else if (viewerCommand->type == catan::debug::ViewerCommandType::SelectTransformation) {
                if (viewerCommand->actionIndex >= actions.size()) {
                    std::cout << "Viewer transformation is no longer available.\n";
                    continue;
                }
                line = std::to_string(viewerCommand->actionIndex);
                std::cout << "Viewer recorded: " << describeAction(actions[viewerCommand->actionIndex], currentState()) << '\n';
            } else if (viewerCommand->type == catan::debug::ViewerCommandType::ObserveVictory) {
                if (!perspective) { std::cout << "Victory observations require perspective mode.\n"; continue; }
                line = viewerCommand->player == catan::PlayerId::One ? "winner p1" : "winner p2";
            } else if (viewerCommand->type == catan::debug::ViewerCommandType::RandomChance) {
                if (chance.empty()) {
                    std::cout << "Viewer random outcome is no longer available.\n";
                    continue;
                }
                line = "random";
                std::cout << "Viewer requested a weighted random outcome.\n";
            } else {
                const auto analysis = liveAnalysis.latest();
                if (!analysis || viewerCommand->actionIndex >= analysis->actions.size()) {
                    std::cout << "Viewer action is no longer available.\n";
                    continue;
                }
                addEngineBatches(*analysis);
                const auto selected = std::find(
                    actions.begin(), actions.end(), analysis->actions[viewerCommand->actionIndex].action);
                if (selected == actions.end()) {
                    std::cout << "Viewer action is no longer legal.\n";
                    continue;
                }
                line = std::to_string(static_cast<std::size_t>(selected - actions.begin()));
                std::cout << "Viewer selected " << line << ": "
                          << describeAction(*selected, currentState()) << '\n';
            }
        } else if (line == "rewind" || line == "undo") {
            rewindOneAction();
            continue;
        }

        auto commandPath = [&](const std::string& command, const std::string& fallback) {
            if (line.size() <= command.size()) return std::filesystem::path(fallback);
            const auto first = line.find_first_not_of(' ', command.size());
            return first == std::string::npos ? std::filesystem::path(fallback)
                                              : std::filesystem::path(line.substr(first));
        };
        if (line == "image" || line.starts_with("image ")) {
            const auto output = commandPath("image", "debug_state.svg");
            std::string error;
            auto displayedAnalysis = liveAnalysis.latest();
            if (!displayedAnalysis) displayedAnalysis = currentAnalysis;
            const bool rendered = displayedAnalysis
                ? (perspective ? catan::debug::renderStateImage(*information[catan::index(*perspective)], *displayedAnalysis, output, &error)
                               : catan::debug::renderStateImage(currentState(), *displayedAnalysis, output, &error))
                : (perspective ? catan::debug::renderStateImage(*information[catan::index(*perspective)], output, &error)
                               : catan::debug::renderStateImage(currentState(), output, &error));
            if (rendered)
                std::cout << "Rendered " << std::filesystem::absolute(output) << '\n';
            else
                std::cout << "Render failed: " << error << '\n';
            continue;
        }
        if (line == "json" || line.starts_with("json ")) {
            const auto output = commandPath("json", "debug_state.json");
            std::string error;
            auto displayedAnalysis = liveAnalysis.latest();
            if (!displayedAnalysis) displayedAnalysis = currentAnalysis;
            const bool exported = displayedAnalysis
                ? (perspective ? catan::debug::writeStateJson(*information[catan::index(*perspective)], *displayedAnalysis, output, &error)
                               : catan::debug::writeStateJson(currentState(), *displayedAnalysis, output, &error))
                : (perspective ? catan::debug::writeStateJson(*information[catan::index(*perspective)], output, &error)
                               : catan::debug::writeStateJson(currentState(), output, &error));
            if (exported)
                std::cout << "Exported " << std::filesystem::absolute(output) << '\n';
            else
                std::cout << "Export failed: " << error << '\n';
            continue;
        }
        std::optional<std::size_t> engineChoice;
        if (line == "mcts" || line.starts_with("mcts ") || line == "best" || line.starts_with("best ")) {
            if (perspective && inputPlayer() != *perspective) {
                std::cout << "Input the opponent's observed action; only your tree is available.\n";
                continue;
            }
            const bool chooseBest = line == "best" || line.starts_with("best ");
            const std::string command = chooseBest ? "best" : "mcts";
            std::size_t iterations = 2'000;
            if (line.size() > command.size()) {
                std::istringstream input(line.substr(command.size()));
                if (!(input >> iterations) || !input.eof() || iterations == 0) {
                    std::cout << "Iterations must be a positive integer.\n";
                    continue;
                }
            }
            catan::MctsConfig config;
            config.iterations = iterations;
            config.seed = seed ^ (currentState().turnNumber * 0x9e3779b9U);
            std::optional<catan::MctsResult> analysisValue;
            if (enableVisualizer) {
                if (chooseBest) liveAnalysis.stop();
                analysisValue = liveAnalysis.latest();
                if (!analysisValue && !chooseBest) {
                    std::cout << "Live MCTS is warming up; its first update will appear shortly.\n";
                    continue;
                }
            }
            if (!analysisValue) {
                const auto observer = perspective.value_or(inputPlayer());
                analysisValue = catan::MctsEngine(config).search(*information[catan::index(observer)]);
            }
            const auto& analysis = *analysisValue;
            currentAnalysis = analysis;
            addEngineBatches(analysis);
            if (enableVisualizer && chooseBest) {
                std::string error;
                if (!visualizer.update(currentState(), analysis, &error))
                    std::cout << "Visualizer analysis update failed: " << error << '\n';
            }
            std::cout << "MCTS from " << catan::toString(analysis.perspective)
                      << " perspective (" << analysis.iterations << " iterations, depth "
                      << analysis.maximumDepth << ")\n";
            for (const auto& evaluation : analysis.actions) {
                const auto candidate = std::find(actions.begin(), actions.end(), evaluation.action);
                const auto actionIndex = static_cast<std::size_t>(std::distance(actions.begin(), candidate));
                std::cout << std::setw(4) << actionIndex << ": "
                          << std::fixed << std::setprecision(2)
                          << evaluation.estimatedWinProbability * 100.0 << "%  visits="
                          << evaluation.visits;
                if (evaluation.chanceProbability)
                    std::cout << "  chance=" << *evaluation.chanceProbability * 100.0 << '%';
                std::cout << "  " << describeAction(evaluation.action, currentState()) << '\n';
            }
            if (analysis.bestAction) {
                const auto selected = std::find(actions.begin(), actions.end(), *analysis.bestAction);
                engineChoice = static_cast<std::size_t>(std::distance(actions.begin(), selected));
                std::cout << "Best: " << *engineChoice << " ("
                          << describeAction(*analysis.bestAction, currentState()) << ")\n";
                if (!chooseBest) continue;
            } else {
                if (analysis.chanceNode)
                    std::cout << "This is a chance node; there is no player-selectable best outcome.\n";
                else
                    std::cout << "There is no legal action to select.\n";
                if (enableVisualizer && chooseBest)
                    liveAnalysis.resume();
                continue;
            }
        }
        const bool observeVictory = perspective && (line == "winner p1" || line == "winner p2");
        if (currentState().turn.phase == catan::Phase::Finished && !observeVictory) {
            std::cout << "The game is finished; use 'image', 'json', or 'q'.\n";
            continue;
        }

        HistoryEntry previous{game ? std::optional(game->state()) : std::nullopt, random, information, viewerEvents};
        std::size_t choice = engineChoice.value_or(actions.size());
        if (!engineChoice && !observeVictory) {
            bool requestRandom = (line == "random" || line == "auto") && !chance.empty();
            bool parsedNumber = false;
            if (!requestRandom) {
                std::istringstream input(line);
                parsedNumber = static_cast<bool>(input >> choice) && input.eof();
                requestRandom = parsedNumber && !chance.empty() && choice == actions.size();
            }
            if (requestRandom) {
                const auto outcomeIndex = sampleChanceOutcome(chance, random);
                const auto selected = std::find(actions.begin(), actions.end(), chance[outcomeIndex].action);
                if (selected != actions.end()) choice = static_cast<std::size_t>(selected - actions.begin());
                std::cout << "Randomly selected " << choice << ": "
                          << describeAction(chance[outcomeIndex].action, currentState())
                          << "  [p=" << std::fixed << std::setprecision(4)
                          << chance[outcomeIndex].probability << "]\n";
            } else if (!parsedNumber) {
                std::cout << "Invalid selection.\n";
                continue;
            }
        }
        if (choice >= actions.size() && !observeVictory) {
            std::cout << "Invalid selection.\n";
            continue;
        }
        catan::Action selectedAction = observeVictory
            ? catan::Action(catan::ObserveVictory{line == "winner p1" ? catan::PlayerId::One : catan::PlayerId::Two})
            : actions[choice];
        if (const auto* discard = std::get_if<catan::DiscardResources>(&selectedAction)) {
            const auto discardingPlayer = inputPlayer();
            pendingDiscardChoices[catan::index(discardingPlayer)] = discard->cards;
            bool ready = true;
            for (std::size_t p = 0; p < catan::PlayerCount; ++p)
                ready &= !currentState().turn.pendingDiscards[p] || pendingDiscardChoices[p].has_value();
            if (!ready) {
                currentAnalysis.reset();
                if (enableVisualizer) {
                    catan::debug::ViewerEvent event;
                    event.id = nextViewerEventId++;
                    event.type = "discard_selection";
                    event.message = std::string(catan::toString(discardingPlayer)) + " selected their discard; waiting for the other player";
                    viewerEvents.push_back(std::move(event));
                    visualizer.setEvents(viewerEvents);
                }
                refreshDiscardInput();
                receipt.accepted = true;
                receipt.message = "Discard choice recorded; waiting for the other player.";
                std::cout << "Discard selection recorded; waiting for the other player. Game state unchanged.\n";
                continue;
            }
            catan::ResolveDiscards joint{};
            for (std::size_t p = 0; p < catan::PlayerCount; ++p)
                if (pendingDiscardChoices[p]) joint.discarded[p] = *pendingDiscardChoices[p];
            selectedAction = joint;
        }
        liveAnalysis.stop();
        currentAnalysis.reset();
        const auto& beforeAction = previous.state ? *previous.state
            : previous.information[catan::index(*perspective)]->state();
        auto result = perspective ? information[catan::index(*perspective)]->transform(selectedAction)
                                  : game->transform(selectedAction);
        receipt.accepted = result.accepted;
        receipt.message = result.accepted ? "Move applied." : result.message;
        if (!result.accepted) std::cout << "Rejected: " << result.message << '\n';
        if (result.accepted) {
            if (std::holds_alternative<catan::ResolveDiscards>(selectedAction)) {
                const auto staged = [](const auto& event) { return event.type == "discard_selection"; };
                std::erase_if(viewerEvents, staged);
                std::erase_if(previous.viewerEvents, staged);
            }
            pendingDiscardChoices = {};
            if (enableVisualizer) visualizer.setDiscardInput(std::nullopt);
            if (!perspective) {
                for (auto& view : information) {
                    catan::Action observation = selectedAction;
                    if (std::holds_alternative<catan::DrawDevelopmentCard>(observation) &&
                        view->decisionPlayer() != view->observer()) observation = catan::DrawUnknownDevelopmentCard{};
                    const auto update = view->transform(observation);
                    if (!update.accepted) throw std::logic_error("Information transformation rejected: " + update.message);
                    if (game->state().winner) {
                        const auto victory = view->transform(catan::ObserveVictory{*game->state().winner});
                        if (!victory.accepted) throw std::logic_error(victory.message);
                    }
                }
            }
            recordViewerEvent(selectedAction, beforeAction, result.events);
            history.push_back(std::move(previous));
        }
        if (enableVisualizer) {
            if (result.accepted) {
                visualizer.setRewindAvailable(true);
                if (perspective) visualizer.setInformationState(*information[catan::index(*perspective)]);
                liveAnalysis.advance(selectedAction, currentState(), information);
            }
            else liveAnalysis.resume();
        }
        for (const auto& event : result.events) std::cout << "  -> " << event << '\n';
        if (result.accepted && perspective && currentState().turn.phase == catan::Phase::DevelopmentDraw &&
            currentState().turn.decisionPlayer != *perspective) {
            liveAnalysis.stop();
            const catan::Action unknown = catan::DrawUnknownDevelopmentCard{};
            const auto beforeUnknown = currentState();
            const auto draw = information[catan::index(*perspective)]->transform(unknown);
            if (!draw.accepted) throw std::logic_error(draw.message);
            recordViewerEvent(unknown, beforeUnknown, draw.events);
            for (const auto& event : draw.events) std::cout << "  -> " << event << '\n';
            if (enableVisualizer) {
                visualizer.setInformationState(*information[catan::index(*perspective)]);
                liveAnalysis.advance(unknown, currentState(), information);
            }
        }
    }
    if (currentState().winner) std::cout << catan::toString(*currentState().winner) << " won.\n";
}
