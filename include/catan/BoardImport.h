#pragma once

#include "catan/Board.h"

#include <filesystem>
#include <optional>
#include <string>

namespace catan {

// Accepts catan-board-v1 JSON, decoded Colonist snapshot JSON, or the JSONL
// output of capture_colonist_ws.mjs. Finds the first complete board snapshot.
// Imports layout only; returns an empty board with the robber on the desert.
// Failure is atomic: no simulator state is created or changed.
std::optional<BoardState> loadColonistBoard(const std::filesystem::path& input,
                                           std::string* error = nullptr);
// Same contract for an inline JSON snapshot or capture record.
std::optional<BoardState> loadColonistBoardJson(const std::string& json,
                                               std::string* error = nullptr);

} // namespace catan
