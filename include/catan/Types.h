#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <numeric>

namespace catan {

constexpr std::size_t PlayerCount = 2;
constexpr std::size_t ResourceCount = 5;
constexpr std::size_t DevelopmentCardTypeCount = 5;

enum class PlayerId : std::uint8_t { One, Two };

constexpr std::size_t index(PlayerId player) { return static_cast<std::size_t>(player); }
constexpr PlayerId opponent(PlayerId player) {
    return player == PlayerId::One ? PlayerId::Two : PlayerId::One;
}

enum class Resource : std::uint8_t { Lumber, Brick, Wool, Grain, Ore };
constexpr std::size_t index(Resource resource) { return static_cast<std::size_t>(resource); }

enum class Terrain : std::uint8_t { Forest, Hills, Pasture, Fields, Mountains, Desert };
enum class BuildingType : std::uint8_t { Settlement, City };
enum class DevelopmentCard : std::uint8_t {
    Knight,
    RoadBuilding,
    YearOfPlenty,
    Monopoly,
    VictoryPoint
};
constexpr std::size_t index(DevelopmentCard card) { return static_cast<std::size_t>(card); }

enum class PortType : std::uint8_t { Generic, Lumber, Brick, Wool, Grain, Ore };

struct HexId {
    std::uint8_t value{};
    auto operator<=>(const HexId&) const = default;
};
struct VertexId {
    std::uint8_t value{};
    auto operator<=>(const VertexId&) const = default;
};
struct EdgeId {
    std::uint8_t value{};
    auto operator<=>(const EdgeId&) const = default;
};

using Resources = std::array<std::uint8_t, ResourceCount>;

constexpr std::uint16_t total(const Resources& resources) {
    std::uint16_t result = 0;
    for (auto amount : resources) result += amount;
    return result;
}

constexpr Resources resourceBundle(Resource resource, std::uint8_t amount = 1) {
    Resources result{};
    result[index(resource)] = amount;
    return result;
}

inline constexpr Resources RoadCost       {1, 1, 0, 0, 0};
inline constexpr Resources SettlementCost {1, 1, 1, 1, 0};
inline constexpr Resources CityCost       {0, 0, 0, 2, 3};
inline constexpr Resources DevelopmentCost{0, 0, 1, 1, 1};

const char* toString(PlayerId player);
const char* toString(Resource resource);
const char* toString(Terrain terrain);
const char* toString(DevelopmentCard card);
const char* toString(PortType port);

} // namespace catan
