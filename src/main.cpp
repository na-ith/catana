#include "catan/Debug.h"
#include "catan/Belief.h"
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

void printState(const catan::GameState& state) {
    std::cout << "\nTurn " << state.turnNumber << " | " << catan::toString(state.turn.phase)
              << " | acting: " << catan::toString(state.turn.decisionPlayer) << '\n';
    for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
        auto player = static_cast<catan::PlayerId>(p);
        std::cout << "  " << catan::toString(player) << ": " << +catan::victoryPoints(state, player)
                  << " VP (" << +catan::publicVictoryPoints(state, player) << " public), resources ";
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
    catan::GameState state;
    std::mt19937 random;
    std::array<catan::DevelopmentBelief, catan::PlayerCount> beliefs;
};

class LiveMctsAnalysis {
public:
    LiveMctsAnalysis(catan::debug::LiveVisualizer& visualizer, std::size_t workerCount)
        : visualizer_(visualizer), workerCount_(std::max<std::size_t>(1, workerCount)) {}
    ~LiveMctsAnalysis() { stop(); }

    LiveMctsAnalysis(const LiveMctsAnalysis&) = delete;
    LiveMctsAnalysis& operator=(const LiveMctsAnalysis&) = delete;

    void startNew(const catan::GameState& state,
                  const std::array<catan::DevelopmentBelief, catan::PlayerCount>& beliefs,
                  std::uint32_t seed) {
        stop();
        publicState_ = state;
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
            auto& tree = trees_[p];
            tree.latest.reset();
            tree.workerResults.assign(workerCount_, std::nullopt);
            tree.sessions.clear();
            tree.sessions.reserve(workerCount_);
            tree.random.seed(seed ^ static_cast<std::uint32_t>((p + 1) * 0x85ebca6bU));
            for (std::size_t worker = 0; worker < workerCount_; ++worker)
                tree.sessions.push_back(makeSession(
                    static_cast<catan::PlayerId>(p), beliefs[p], state, seed, worker));
            visualizer_.setTreeRunning(static_cast<catan::PlayerId>(p), tree.enabled);
        }
        baseSeed_ = seed;
        resume();
    }

    void resume() {
        stop();
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) resumeTree(p);
    }

    bool advance(const catan::Action& action, const catan::GameState& state,
                 const std::array<catan::DevelopmentBelief, catan::PlayerCount>& beliefs) {
        stop();
        bool reused = false;
        publicState_ = state;
        for (std::size_t p = 0; p < catan::PlayerCount; ++p) {
            auto& tree = trees_[p];
            std::vector<catan::MctsResult> results;
            results.reserve(workerCount_);
            for (std::size_t worker = 0; worker < tree.sessions.size(); ++worker) {
                auto& session = tree.sessions[worker];
                bool compatible = true;
                catan::Action observedAction = action;
                if (std::holds_alternative<catan::DrawDevelopmentCard>(action) &&
                    session->state().turn.decisionPlayer != static_cast<catan::PlayerId>(p)) {
                    const auto outcomes = catan::Game(session->state()).chanceOutcomes();
                    if (outcomes.empty()) compatible = false;
                    else observedAction = outcomes[sampleChanceOutcome(outcomes, tree.random)].action;
                }
                if (compatible) {
                    try {
                        reused |= session->advance(observedAction);
                    } catch (const std::invalid_argument&) {
                        compatible = false;
                    }
                }
                if (compatible)
                    compatible = beliefs[p].contains(session->state()) &&
                                 publicStateMatches(session->state(), state);
                if (!compatible)
                    session = makeSession(static_cast<catan::PlayerId>(p), beliefs[p], state,
                                          baseSeed_, worker);
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
        return trees_[catan::index(publicState_.turn.decisionPlayer)].latest;
    }

    void setEnabled(catan::PlayerId player, bool enabled) {
        const auto p = catan::index(player);
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
        std::vector<std::unique_ptr<catan::MctsSession>> sessions;
        std::vector<std::jthread> workers;
        std::mt19937 random;
        std::chrono::steady_clock::time_point lastPublished{};
    };

    static bool publicStateMatches(catan::GameState candidate, const catan::GameState& actual) {
        for (std::size_t p = 0; p < catan::PlayerCount; ++p)
            candidate.players[p].developmentCards = actual.players[p].developmentCards;
        candidate.developmentDeck = actual.developmentDeck;
        return candidate == actual;
    }

    std::unique_ptr<catan::MctsSession> makeSession(
        catan::PlayerId perspective, const catan::DevelopmentBelief& belief,
        const catan::GameState& state, std::uint32_t seed, std::size_t worker) const {
        catan::MctsConfig config;
        config.iterations = std::numeric_limits<std::size_t>::max();
        config.progressInterval = 100;
        config.seed = seed + static_cast<std::uint32_t>(worker * 0x9e3779b9U) +
                      static_cast<std::uint32_t>((catan::index(perspective) + 1) * 0x85ebca6bU);
        const double quantile = (static_cast<double>(worker) + 0.5) /
                                static_cast<double>(workerCount_);
        return std::make_unique<catan::MctsSession>(
            belief.determinize(state, quantile), perspective, config);
    }

    static std::optional<catan::MctsResult> mergeCompatible(
        const std::vector<catan::MctsResult>& results) {
        if (results.empty()) return std::nullopt;
        try {
            return catan::mergeMctsResults(results);
        } catch (const std::invalid_argument&) {
            // When the decision belongs to the opponent, determinizations can
            // expose different legal development-card plays. There is no one
            // public root action list to display for that observer's tree.
            return std::nullopt;
        }
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
                if (combined && player == catan::index(publicState_.turn.decisionPlayer)) {
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
            analysis = trees_[catan::index(state.turn.decisionPlayer)].latest;
            ++publishGeneration_;
        }
        std::string ignored;
        if (analysis) visualizer_.update(state, *analysis, &ignored);
        else visualizer_.update(state, &ignored);
    }

    catan::debug::LiveVisualizer& visualizer_;
    const std::size_t workerCount_;
    mutable std::mutex mutex_;
    std::mutex publishMutex_;
    std::array<PlayerTree, catan::PlayerCount> trees_;
    catan::GameState publicState_;
    std::uint32_t baseSeed_{};
    std::size_t publishGeneration_{};
};

} // namespace

int main(int argc, char** argv) {
    std::uint32_t seed = 1;
    bool seedWasSet = false;
    bool uninitialized = false;
    std::optional<std::filesystem::path> boardFile;
    bool enableVisualizer = false;
    std::size_t mctsThreads = std::clamp<unsigned>(std::thread::hardware_concurrency(), 1U, 8U);
    for (int argument = 1; argument < argc; ++argument) {
        const std::string value = argv[argument];
        if (value == "--visualizer") {
            enableVisualizer = true;
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
                         "[--visualizer] [--mcts-threads N]\n"
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
    catan::Game game = std::move(*initializedGame);
    catan::debug::LiveVisualizer visualizer;
    if (enableVisualizer) {
        std::string error;
        if (!visualizer.start(game.state(), &error)) {
            std::cerr << "Could not start live visualizer: " << error << '\n';
            return 2;
        }
    }
    std::mt19937 random(seed ^ 0x9e3779b9U);
    std::array<catan::DevelopmentBelief, catan::PlayerCount> beliefs{
        catan::DevelopmentBelief(catan::PlayerId::One, game.state()),
        catan::DevelopmentBelief(catan::PlayerId::Two, game.state()),
    };
    std::vector<HistoryEntry> history;
    std::optional<catan::MctsResult> currentAnalysis;
    LiveMctsAnalysis liveAnalysis(visualizer, mctsThreads);
    if (enableVisualizer)
        liveAnalysis.startNew(game.state(), beliefs, seed ^ 0x517cc1b7U);
    std::cout << "Colonist ranked 1v1 stepper (";
    if (boardFile) std::cout << "imported board " << *boardFile;
    else std::cout << "board seed " << seed;
    std::cout << ")\n"
                 "Choose an action number; chance nodes also offer a weighted 'random' option,\n"
                 "'mcts [iterations]' to analyse actions, 'best [iterations]' to play the best action,\n"
                 "'rewind' to undo one action, 'image [file.svg]' to render the state,\n"
                 "'json [file.json]' to export it, or 'q' to quit.\n";
    if (enableVisualizer)
        std::cout << "Both player-specific MCTS belief trees are running continuously with "
                  << mctsThreads << " workers per enabled tree.\n";

    while (true) {
        printState(game.state());
        auto actions = game.legalActions();
        actions.erase(std::remove_if(actions.begin(), actions.end(), [](const auto& action) {
            return std::holds_alternative<catan::Resign>(action);
        }), actions.end());
        const auto chance = game.chanceOutcomes();
        for (std::size_t i = 0; i < actions.size(); ++i) {
            std::cout << std::setw(4) << i << ": " << describeAction(actions[i], game.state());
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
        if (!viewerCommand && (line == "q" || line == "quit")) break;
        if (!viewerCommand && line == "status") {
            std::cout << "State: initialized | " << catan::toString(game.state().turn.phase)
                      << " | acting: " << catan::toString(game.decisionPlayer()) << '\n';
            continue;
        }
        if (!viewerCommand && (line.starts_with("init ") || line.starts_with("init-json "))) {
            std::cout << "Already initialized; start a new --uninitialized process to import another board.\n";
            continue;
        }

        auto rewindOneAction = [&]() {
            if (history.empty()) {
                std::cout << "Nothing to rewind.\n";
                return;
            }
            liveAnalysis.stop();
            auto previous = std::move(history.back());
            history.pop_back();
            game = catan::Game(std::move(previous.state));
            random = previous.random;
            beliefs = std::move(previous.beliefs);
            currentAnalysis.reset();
            if (enableVisualizer) {
                visualizer.setRewindAvailable(!history.empty());
                std::string error;
                if (!visualizer.update(game.state(), &error))
                    std::cout << "Visualizer rewind update failed: " << error << '\n';
                liveAnalysis.startNew(
                    game.state(), beliefs,
                    seed ^ 0x517cc1b7U ^ static_cast<std::uint32_t>(history.size()));
            }
            std::cout << "Rewound one action.\n";
        };

        if (viewerCommand) {
            if (viewerCommand->positionVersion != visualizer.positionVersion()) {
                std::cout << "Ignored a stale viewer command.\n";
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::Rewind) {
                rewindOneAction();
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::ToggleTree) {
                liveAnalysis.setEnabled(viewerCommand->player, viewerCommand->enabled);
                std::cout << catan::toString(viewerCommand->player) << " belief-tree calculation "
                          << (viewerCommand->enabled ? "enabled" : "paused") << ".\n";
                continue;
            }
            if (viewerCommand->type == catan::debug::ViewerCommandType::RandomChance) {
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
                const auto selected = std::find(
                    actions.begin(), actions.end(), analysis->actions[viewerCommand->actionIndex].action);
                if (selected == actions.end()) {
                    std::cout << "Viewer action is no longer legal.\n";
                    continue;
                }
                line = std::to_string(static_cast<std::size_t>(selected - actions.begin()));
                std::cout << "Viewer selected " << line << ": "
                          << describeAction(*selected, game.state()) << '\n';
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
                ? catan::debug::renderStateImage(game.state(), *displayedAnalysis, output, &error)
                : catan::debug::renderStateImage(game.state(), output, &error);
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
                ? catan::debug::writeStateJson(game.state(), *displayedAnalysis, output, &error)
                : catan::debug::writeStateJson(game.state(), output, &error);
            if (exported)
                std::cout << "Exported " << std::filesystem::absolute(output) << '\n';
            else
                std::cout << "Export failed: " << error << '\n';
            continue;
        }
        std::optional<std::size_t> engineChoice;
        if (line == "mcts" || line.starts_with("mcts ") || line == "best" || line.starts_with("best ")) {
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
            config.seed = seed ^ (game.state().turnNumber * 0x9e3779b9U);
            std::optional<catan::MctsResult> analysisValue;
            if (enableVisualizer) {
                if (chooseBest) liveAnalysis.stop();
                analysisValue = liveAnalysis.latest();
                if (!analysisValue && !chooseBest) {
                    std::cout << "Live MCTS is warming up; its first update will appear shortly.\n";
                    continue;
                }
            }
            if (!analysisValue) analysisValue = catan::MctsEngine(config).search(game.state());
            const auto& analysis = *analysisValue;
            currentAnalysis = analysis;
            if (enableVisualizer && chooseBest) {
                std::string error;
                if (!visualizer.update(game.state(), analysis, &error))
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
                std::cout << "  " << describeAction(evaluation.action, game.state()) << '\n';
            }
            if (analysis.bestAction) {
                const auto selected = std::find(actions.begin(), actions.end(), *analysis.bestAction);
                engineChoice = static_cast<std::size_t>(std::distance(actions.begin(), selected));
                std::cout << "Best: " << *engineChoice << " ("
                          << describeAction(*analysis.bestAction, game.state()) << ")\n";
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
        if (game.state().turn.phase == catan::Phase::Finished) {
            std::cout << "The game is finished; use 'image', 'json', or 'q'.\n";
            continue;
        }

        const HistoryEntry previous{game.state(), random, beliefs};
        std::size_t choice = engineChoice.value_or(actions.size());
        if (!engineChoice) {
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
                          << describeAction(chance[outcomeIndex].action, game.state())
                          << "  [p=" << std::fixed << std::setprecision(4)
                          << chance[outcomeIndex].probability << "]\n";
            } else if (!parsedNumber) {
                std::cout << "Invalid selection.\n";
                continue;
            }
        }
        if (choice >= actions.size()) {
            std::cout << "Invalid selection.\n";
            continue;
        }
        const auto selectedAction = actions[choice];
        liveAnalysis.stop();
        currentAnalysis.reset();
        auto result = game.step(selectedAction);
        if (!result.accepted) std::cout << "Rejected: " << result.message << '\n';
        if (result.accepted) {
            for (auto& belief : beliefs)
                belief.observe(previous.state, selectedAction, game.state());
            history.push_back(previous);
        }
        if (enableVisualizer) {
            if (result.accepted) {
                visualizer.setRewindAvailable(true);
                liveAnalysis.advance(selectedAction, game.state(), beliefs);
            }
            else liveAnalysis.resume();
        }
        for (const auto& event : result.events) std::cout << "  -> " << event << '\n';
    }
    if (game.state().winner) std::cout << catan::toString(*game.state().winner) << " won.\n";
}
