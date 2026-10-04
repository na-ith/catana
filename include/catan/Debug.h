#pragma once

#include "catan/Mcts.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace catan::debug {

enum class ViewerCommandType { SelectAction, SelectTransformation, ObserveVictory, RandomChance, Rewind, ToggleTree };

struct ViewerCommand {
    ViewerCommandType type{ViewerCommandType::SelectAction};
    std::uint64_t positionVersion{};
    std::size_t actionIndex{};
    PlayerId player{PlayerId::One};
    bool enabled{};
};

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

// Long-running debug viewer. start() launches the Python localhost viewer in
// a browser window; update() atomically publishes subsequent states.
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
    std::optional<ViewerCommand> takeCommand(std::string* error = nullptr);
    std::uint64_t positionVersion() const;
    bool running() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace catan::debug
