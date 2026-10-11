#include "catan/Board.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

namespace catan {
namespace {

template <typename T, std::size_t N>
void addToFirstEmpty(std::array<std::optional<T>, N>& values, T value) {
    for (auto& slot : values) {
        if (!slot) {
            slot = value;
            return;
        }
    }
    throw std::logic_error("topology degree exceeded");
}

const auto& numberTokenSpirals() {
    static const auto spirals = [] {
        // Axial coordinates, counterclockwise in the viewer (positive y is down).
        // Complete the outer ring, then the inner ring, then the center.
        constexpr std::array<std::pair<int, int>, BoardTopology::HexCount> spiral{{
            {2, 0}, {2, -1}, {2, -2}, {1, -2}, {0, -2}, {-1, -1},
            {-2, 0}, {-2, 1}, {-2, 2}, {-1, 2}, {0, 2}, {1, 1},
            {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {-1, 1}, {0, 1}, {0, 0}
        }};
        std::array<std::array<HexId, BoardTopology::HexCount>, 6> result{};
        const auto& hexes = BoardTopology::standard().hexes;
        for (std::size_t rotation = 0; rotation < result.size(); ++rotation) {
            for (std::size_t position = 0; position < spiral.size(); ++position) {
                auto [q, r] = spiral[position];
                for (std::size_t step = 0; step < rotation; ++step) {
                    const auto nextQ = -r;
                    r += q;
                    q = nextQ;
                }
                const auto hex = std::find_if(hexes.begin(), hexes.end(), [&](const auto& value) {
                    return value.center.x == 3 * q && value.center.y == 2 * r + q;
                });
                if (hex == hexes.end()) throw std::logic_error("invalid number-token spiral");
                result[rotation][position] = HexId{static_cast<std::uint8_t>(hex - hexes.begin())};
            }
        }
        return result;
    }();
    return spirals;
}

} // namespace

BoardTopology::BoardTopology() {
    using Point = std::pair<int, int>;
    const std::array<Point, 6> cornerOffsets{{
        {2, 0}, {1, 1}, {-1, 1}, {-2, 0}, {-1, -1}, {1, -1}
    }};

    std::vector<Point> centers;
    for (int r = -2; r <= 2; ++r) {
        const int qMin = std::max(-2, -r - 2);
        const int qMax = std::min(2, -r + 2);
        for (int q = qMin; q <= qMax; ++q) centers.emplace_back(3 * q, 2 * r + q);
    }
    if (centers.size() != HexCount) throw std::logic_error("invalid hex topology");

    std::map<Point, VertexId> vertexIds;
    std::array<Point, VertexCount> vertexPoints{};
    std::map<std::pair<std::uint8_t, std::uint8_t>, EdgeId> edgeIds;

    for (std::size_t h = 0; h < centers.size(); ++h) {
        hexes[h].center = {static_cast<std::int16_t>(centers[h].first),
                           static_cast<std::int16_t>(centers[h].second)};
        for (std::size_t corner = 0; corner < 6; ++corner) {
            Point point{centers[h].first + cornerOffsets[corner].first,
                        centers[h].second + cornerOffsets[corner].second};
            auto [it, inserted] = vertexIds.emplace(point, VertexId{static_cast<std::uint8_t>(vertexIds.size())});
            const VertexId vertex = it->second;
            if (inserted) {
                vertexPoints[vertex.value] = point;
                vertices[vertex.value].position = {static_cast<std::int16_t>(point.first),
                                                    static_cast<std::int16_t>(point.second)};
            }
            hexes[h].vertices[corner] = vertex;
            addToFirstEmpty(vertices[vertex.value].hexes, HexId{static_cast<std::uint8_t>(h)});
        }
    }
    if (vertexIds.size() != VertexCount) throw std::logic_error("invalid vertex topology");

    for (std::size_t h = 0; h < HexCount; ++h) {
        for (std::size_t side = 0; side < 6; ++side) {
            VertexId a = hexes[h].vertices[side];
            VertexId b = hexes[h].vertices[(side + 1) % 6];
            auto key = std::minmax(a.value, b.value);
            auto [it, inserted] = edgeIds.emplace(key, EdgeId{static_cast<std::uint8_t>(edgeIds.size())});
            EdgeId edge = it->second;
            hexes[h].edges[side] = edge;
            if (inserted) {
                edges[edge.value].vertices = {a, b};
                addToFirstEmpty(vertices[a.value].edges, edge);
                addToFirstEmpty(vertices[b.value].edges, edge);
                addToFirstEmpty(vertices[a.value].neighbours, b);
                addToFirstEmpty(vertices[b.value].neighbours, a);
            }
            addToFirstEmpty(edges[edge.value].hexes, HexId{static_cast<std::uint8_t>(h)});
        }
    }
    if (edgeIds.size() != EdgeCount) throw std::logic_error("invalid edge topology");

    std::vector<std::pair<double, EdgeId>> boundary;
    for (std::size_t e = 0; e < EdgeCount; ++e) {
        if (edges[e].hexes[1]) continue;
        const auto a = vertexPoints[edges[e].vertices[0].value];
        const auto b = vertexPoints[edges[e].vertices[1].value];
        boundary.emplace_back(std::atan2(a.second + b.second, a.first + b.first),
                              EdgeId{static_cast<std::uint8_t>(e)});
    }
    std::sort(boundary.begin(), boundary.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    constexpr std::array<std::size_t, PortCount> portPositions{0, 3, 7, 10, 13, 17, 20, 23, 27};
    for (std::size_t i = 0; i < PortCount; ++i) portEdges[i] = boundary[portPositions[i]].second;
}

const BoardTopology& BoardTopology::standard() {
    static const BoardTopology topology;
    return topology;
}

std::optional<Resource> producedResource(Terrain terrain) {
    switch (terrain) {
    case Terrain::Forest: return Resource::Lumber;
    case Terrain::Hills: return Resource::Brick;
    case Terrain::Pasture: return Resource::Wool;
    case Terrain::Fields: return Resource::Grain;
    case Terrain::Mountains: return Resource::Ore;
    case Terrain::Desert: return std::nullopt;
    }
    return std::nullopt;
}

BoardState makeStandardBoard(std::uint32_t seed) {
    BoardState board;
    std::mt19937 random(seed);
    std::array<Terrain, BoardTopology::HexCount> terrains{
        Terrain::Forest, Terrain::Forest, Terrain::Forest, Terrain::Forest,
        Terrain::Hills, Terrain::Hills, Terrain::Hills,
        Terrain::Pasture, Terrain::Pasture, Terrain::Pasture, Terrain::Pasture,
        Terrain::Fields, Terrain::Fields, Terrain::Fields, Terrain::Fields,
        Terrain::Mountains, Terrain::Mountains, Terrain::Mountains,
        Terrain::Desert
    };
    // Standard A-R token order: our inferred approximation of Colonist's Base map.
    constexpr std::array<std::uint8_t, 18> tokens{
        5, 2, 6, 3, 8, 10, 9, 12, 11, 4, 8, 10, 9, 4, 5, 6, 3, 11
    };
    std::shuffle(terrains.begin(), terrains.end(), random);
    for (std::size_t h = 0; h < BoardTopology::HexCount; ++h)
        board.hexes[h].terrain = terrains[h];

    const auto rotation = std::uniform_int_distribution<std::size_t>(0, 5)(random);
    std::size_t token = 0;
    for (const auto hex : numberTokenSpirals()[rotation]) {
        if (board.hexes[hex.value].terrain == Terrain::Desert) {
            board.hexes[hex.value].number = 0;
            board.robberHex = hex;
        } else {
            board.hexes[hex.value].number = tokens[token++];
        }
    }

    std::array<PortType, BoardTopology::PortCount> ports{
        PortType::Generic, PortType::Generic, PortType::Generic, PortType::Generic,
        PortType::Lumber, PortType::Brick, PortType::Wool, PortType::Grain, PortType::Ore
    };
    std::shuffle(ports.begin(), ports.end(), random);
    const auto& topology = BoardTopology::standard();
    for (std::size_t i = 0; i < ports.size(); ++i) board.ports[i] = {topology.portEdges[i], ports[i]};
    return board;
}

bool validateInitialBoard(const BoardState& board, std::string* reason) {
    const auto fail = [&](const char* message) {
        if (reason) *reason = message;
        return false;
    };
    std::array<unsigned, 6> terrainCounts{}, portCounts{};
    std::array<unsigned, 13> tokenCounts{};
    for (const auto& tile : board.hexes) {
        const auto terrain = static_cast<std::size_t>(tile.terrain);
        if (terrain >= terrainCounts.size()) return fail("invalid terrain type");
        ++terrainCounts[terrain];
        if (tile.terrain == Terrain::Desert) {
            if (tile.number != 0) return fail("desert must have no number token");
        } else if (tile.number < 2 || tile.number > 12 || tile.number == 7) {
            return fail("invalid number token");
        }
        ++tokenCounts[tile.number];
    }
    if (terrainCounts != std::array<unsigned, 6>{4, 3, 4, 4, 3, 1})
        return fail("board must have the standard terrain inventory");
    if (tokenCounts != std::array<unsigned, 13>{1, 0, 1, 2, 2, 2, 2, 0, 2, 2, 2, 2, 1})
        return fail("board must have the standard number-token inventory");
    if (board.robberHex.value >= BoardTopology::HexCount ||
        board.hexes[board.robberHex.value].terrain != Terrain::Desert)
        return fail("initial robber must be on the desert");
    const auto& topology = BoardTopology::standard();
    std::array<bool, BoardTopology::VertexCount> usedVertices{};
    for (const auto& port : board.ports) {
        const auto type = static_cast<std::size_t>(port.type);
        if (type >= portCounts.size()) return fail("invalid port type");
        ++portCounts[type];
        if (port.edge.value >= BoardTopology::EdgeCount)
            return fail("invalid port edge");
        const auto& edge = topology.edges[port.edge.value];
        if (edge.hexes[1]) return fail("port must be on a coastal edge");
        for (auto vertex : edge.vertices) {
            if (usedVertices[vertex.value]) return fail("ports overlap at a vertex");
            usedVertices[vertex.value] = true;
        }
    }
    if (portCounts != std::array<unsigned, 6>{4, 1, 1, 1, 1, 1})
        return fail("board must have four generic and five resource ports");
    for (const auto& building : board.buildings)
        if (building) return fail("initial board contains buildings");
    for (const auto& road : board.roads)
        if (road) return fail("initial board contains roads");
    if (reason) reason->clear();
    return true;
}

} // namespace catan
