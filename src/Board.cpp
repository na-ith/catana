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
    std::array<std::uint8_t, 18> tokens{2, 3, 3, 4, 4, 5, 5, 6, 6, 8, 8, 9, 9, 10, 10, 11, 11, 12};
    const auto& topology = BoardTopology::standard();

    for (;;) {
        std::shuffle(terrains.begin(), terrains.end(), random);
        std::shuffle(tokens.begin(), tokens.end(), random);
        std::size_t token = 0;
        for (std::size_t h = 0; h < BoardTopology::HexCount; ++h) {
            board.hexes[h].terrain = terrains[h];
            if (terrains[h] == Terrain::Desert) {
                board.hexes[h].number = 0;
                board.robberHex = HexId{static_cast<std::uint8_t>(h)};
            } else {
                board.hexes[h].number = tokens[token++];
            }
        }
        bool adjacentRed = false;
        for (const auto& edge : topology.edges) {
            if (!edge.hexes[0] || !edge.hexes[1]) continue;
            auto isRed = [&](HexId id) {
                const auto n = board.hexes[id.value].number;
                return n == 6 || n == 8;
            };
            if (isRed(*edge.hexes[0]) && isRed(*edge.hexes[1])) {
                adjacentRed = true;
                break;
            }
        }
        if (!adjacentRed) break;
    }

    std::array<PortType, BoardTopology::PortCount> ports{
        PortType::Generic, PortType::Generic, PortType::Generic, PortType::Generic,
        PortType::Lumber, PortType::Brick, PortType::Wool, PortType::Grain, PortType::Ore
    };
    std::shuffle(ports.begin(), ports.end(), random);
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
