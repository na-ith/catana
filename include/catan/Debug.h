#pragma once

#include "catan/Mcts.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace catan::debug {

enum class ViewerCommandType { SelectMove, SelectAction, SelectTransformation, ObserveVictory, RandomChance, Rewind, ToggleTree };

struct ViewerCommand {
    ViewerCommandType type{ViewerCommandType::SelectAction};
    std::uint64_t positionVersion{};
    std::size_t actionIndex{};
    PlayerId player{PlayerId::One};
    bool enabled{};
    std::string moveId;
    std::uint64_t requestId{};
};

struct ViewerEvent {
    std::uint64_t id{};
    std::string type;
    std::string message;
    std::vector<std::string> details;
    std::optional<std::uint8_t> diceTotal;
    std::array<Resources, PlayerCount> production{};
};

// Uses the observed before/after states; hidden development draws stay hidden.
ViewerEvent describeViewerEvent(const Action& action, const GameState& before,
                               const GameState& after, const std::vector<std::string>& engineEvents);

// Debug-only, omniscient snapshot format. This is deliberately separate from
// the simulation model and is not intended as a stable save-game format.
bool writeStateJson(const GameState& state,
                    const std::filesystem::path& output,
                    std::string* error = nullptr);
bool writeStateJson(const GameState& state,
                    const MctsResult& analysis,
                    const std::filesystem::path& output,
                    std::string* error = nullptr);

// Writes a temporary JSON snapshot and invokes tools/render_state.py.
// SVG is dependency-free; the renderer reports an error for other formats.
bool renderStateImage(const GameState& state,
                      const std::filesystem::path& output,
                      std::string* error = nullptr);
bool renderStateImage(const GameState& state,
                      const MctsResult& analysis,
                      const std::filesystem::path& output,
                      std::string* error = nullptr);
bool writeStateJson(const InformationState& state, const std::filesystem::path& output,
                    std::string* error = nullptr);
bool writeStateJson(const InformationState& state, const MctsResult& analysis,
                    const std::filesystem::path& output, std::string* error = nullptr);
bool renderStateImage(const InformationState& state, const std::filesystem::path& output,
                      std::string* error = nullptr);
bool renderStateImage(const InformationState& state, const MctsResult& analysis,
                      const std::filesystem::path& output, std::string* error = nullptr);

// Long-running debug viewer. start() publishes immutable layout and launches
// the Python localhost viewer. update() publishes position/input changes and
// analysis separately; unchanged positions reuse their legal-action catalog.
class LiveVisualizer {
public:
    LiveVisualizer();
    ~LiveVisualizer();
    LiveVisualizer(LiveVisualizer&&) noexcept;
    LiveVisualizer& operator=(LiveVisualizer&&) noexcept;
    LiveVisualizer(const LiveVisualizer&) = delete;
    LiveVisualizer& operator=(const LiveVisualizer&) = delete;

    bool start(const GameState& state, std::string* error = nullptr);
    bool update(const GameState& state, std::string* error = nullptr);
    bool update(const GameState& state, const MctsResult& analysis, std::string* error = nullptr);
    void setRewindAvailable(bool available);
    void setTreeRunning(PlayerId player, bool running);
    // Supply masked development-card display and exact posterior summaries.
    void setInformationState(const InformationState& state);
    void setDiscardInput(std::optional<PlayerId> player,
                         std::array<bool, PlayerCount> selected = {});
    void setEvents(const std::vector<ViewerEvent>& events);
    std::optional<ViewerCommand> takeCommand(std::string* error = nullptr);
    // IDs identify published typed actions, independently of analysis ordering.
    std::optional<Action> resolveMove(const std::string& id, std::uint64_t positionVersion) const;
    void acknowledge(const ViewerCommand& command, bool accepted, const std::string& message);
    std::uint64_t positionVersion() const;
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace catan::debug
