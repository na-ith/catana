#include "catan/BoardImport.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#ifndef CATAN_IMPORT_SCRIPT
#define CATAN_IMPORT_SCRIPT "tools/import_colonist_board.py"
#endif
#ifndef CATAN_PYTHON_EXECUTABLE
#define CATAN_PYTHON_EXECUTABLE "python3"
#endif

namespace catan {
namespace {

std::string shellQuote(const std::string& value) {
    std::string result{"'"};
    for (char character : value) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + '\'';
}

} // namespace

std::optional<BoardState> loadColonistBoard(const std::filesystem::path& input, std::string* error) {
    const auto fail = [&](const std::string& reason) -> std::optional<BoardState> {
        if (error) *error = reason;
        return std::nullopt;
    };
    const auto command = shellQuote(CATAN_PYTHON_EXECUTABLE) + " " +
        shellQuote(CATAN_IMPORT_SCRIPT) + " " + shellQuote(input.string()) + " --engine-text";
#if defined(_WIN32)
    auto* pipe = _popen(command.c_str(), "r");
#else
    auto* pipe = popen(command.c_str(), "r");
#endif
    if (!pipe) return fail("could not start Colonist board adapter");
    std::string text;
    std::array<char, 4096> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe)) text += buffer.data();
#if defined(_WIN32)
    const int status = _pclose(pipe);
#else
    const int status = pclose(pipe);
#endif
    if (status != 0) return fail(text.empty() ? "board adapter failed" : text);
    std::istringstream stream(text);
    std::string tag;
    if (!(stream >> tag) || tag != "CATAN_BOARD_V1") return fail("invalid board adapter response");
    BoardState board;
    std::array<bool, BoardTopology::HexCount> seen{};
    for (std::size_t tile = 0; tile < BoardTopology::HexCount; ++tile) {
        int id, terrain, number;
        if (!(stream >> tag >> id >> terrain >> number) || tag != "hex" ||
            id < 0 || id >= static_cast<int>(seen.size()) || seen[id] ||
            terrain < 0 || terrain > 5 || number < 0 || number > 12)
            return fail("invalid or duplicate hex in imported board");
        seen[id] = true;
        board.hexes[id] = {static_cast<Terrain>(terrain), static_cast<std::uint8_t>(number)};
    }
    for (auto& port : board.ports) {
        int edge, type;
        if (!(stream >> tag >> edge >> type) || tag != "port" || edge < 0 ||
            edge >= static_cast<int>(BoardTopology::EdgeCount) || type < 0 || type > 5)
            return fail("invalid port in imported board");
        port = {EdgeId{static_cast<std::uint8_t>(edge)}, static_cast<PortType>(type)};
    }
    int robber;
    if (!(stream >> tag >> robber) || tag != "robber" || robber < 0 ||
        robber >= static_cast<int>(BoardTopology::HexCount)) return fail("invalid initial robber hex");
    board.robberHex = HexId{static_cast<std::uint8_t>(robber)};
    if (stream >> tag) return fail("unexpected trailing board adapter output");
    std::string reason;
    if (!validateInitialBoard(board, &reason)) return fail(reason);
    if (error) error->clear();
    return board;
}

std::optional<BoardState> loadColonistBoardJson(const std::string& json, std::string* error) {
#if defined(_WIN32)
    if (error) *error = "inline import is currently supported on macOS and Linux; use file import";
    return std::nullopt;
#else
    std::error_code filesystemError;
    const auto temporaryRoot = std::filesystem::temp_directory_path(filesystemError);
    if (filesystemError) {
        if (error) *error = "could not locate temporary directory";
        return std::nullopt;
    }
    const auto pattern = (temporaryRoot / "catan-board-import-XXXXXX").string();
    std::vector<char> filename(pattern.begin(), pattern.end());
    filename.push_back('\0');
    const int descriptor = mkstemp(filename.data());
    if (descriptor == -1) {
        if (error) *error = "could not create temporary import file";
        return std::nullopt;
    }
    struct TemporaryFile {
        std::filesystem::path path;
        ~TemporaryFile() {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } temporary{filename.data()};
    FILE* file = fdopen(descriptor, "w");
    if (!file) {
        close(descriptor);
        if (error) *error = "could not open temporary import file";
        return std::nullopt;
    }
    const bool wrote = std::fwrite(json.data(), 1, json.size(), file) == json.size();
    const int closed = std::fclose(file);
    if (!wrote || closed != 0) {
        if (error) *error = "could not write inline board JSON";
        return std::nullopt;
    }
    return loadColonistBoard(temporary.path, error);
#endif
}

} // namespace catan
