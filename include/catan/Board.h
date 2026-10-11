#pragma once

#include "catan/Types.h"

#include <array>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace catan {

struct BoardTopology {
    static constexpr std::size_t VertexCount = 54;
    static constexpr std::size_t EdgeCount = 72;
    static constexpr std::size_t HexCount = 19;
    static constexpr std::size_t PortCount = 9;

    struct Point {
        std::int16_t x{};
        std::int16_t y{};
    };
    struct Hex {
        std::array<VertexId, 6> vertices{};
        std::array<EdgeId, 6> edges{};
        Point center{};
    };
    struct Vertex {
        std::array<std::optional<HexId>, 3> hexes{};
        std::array<std::optional<EdgeId>, 3> edges{};
        std::array<std::optional<VertexId>, 3> neighbours{};
        Point position{};
    };
    struct Edge {
        std::array<VertexId, 2> vertices{};
        std::array<std::optional<HexId>, 2> hexes{};
    };

    std::array<Hex, HexCount> hexes{};
    std::array<Vertex, VertexCount> vertices{};
    std::array<Edge, EdgeCount> edges{};
    std::array<EdgeId, PortCount> portEdges{};

    static const BoardTopology& standard();

private:
    BoardTopology();
};

struct HexState {
    Terrain terrain{Terrain::Desert};
    std::uint8_t number{};
    bool operator==(const HexState&) const = default;
};

struct Building {
    PlayerId owner{PlayerId::One};
    BuildingType type{BuildingType::Settlement};
    bool operator==(const Building&) const = default;
};

struct Road {
    PlayerId owner{PlayerId::One};
    bool operator==(const Road&) const = default;
};

struct PortState {
    EdgeId edge{};
    PortType type{PortType::Generic};
    bool operator==(const PortState&) const = default;
};

struct BoardState {
    std::array<HexState, BoardTopology::HexCount> hexes{};
    std::array<std::optional<Building>, BoardTopology::VertexCount> buildings{};
    std::array<std::optional<Road>, BoardTopology::EdgeCount> roads{};
    std::array<PortState, BoardTopology::PortCount> ports{};
    HexId robberHex{};
    bool operator==(const BoardState&) const = default;
};

// Seeded terrain/port shuffles and standard A-R numbers in one of six spirals.
// Approximates Colonist's Base map; its exact generator is not published.
BoardState makeStandardBoard(std::uint32_t seed);
// Validate a complete, empty standard board before starting initial placement.
bool validateInitialBoard(const BoardState& board, std::string* reason = nullptr);
std::optional<Resource> producedResource(Terrain terrain);

} // namespace catan
