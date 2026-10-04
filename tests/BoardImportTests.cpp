#include "TestHelpers.h"

using namespace catan;
using namespace test_support;

TEST(imported_board_starts_with_fresh_bank_dice_development_deck_and_snake_setup) {
    const auto board = makeStandardBoard(481);
    Game imported(board);
    CHECK_EQ(imported.state(), Game(481).state());
    finishPlacements(imported);
    CHECK_EQ(imported.state().turn.phase, Phase::AwaitRoll);
    CHECK_EQ(buildingCount(imported.state(), PlayerId::One), 2U);
    CHECK_EQ(buildingCount(imported.state(), PlayerId::Two), 2U);
    CHECK(validateState(imported.state()));
}

TEST(initial_board_validation_rejects_bad_inventories_ports_and_pieces) {
    const auto original = makeStandardBoard(98);
    std::string reason;
    REQUIRE(validateInitialBoard(original, &reason));
    auto bad = original;
    bad.hexes[0].terrain = static_cast<Terrain>(255);
    CHECK(!validateInitialBoard(bad, &reason));
    CHECK(!reason.empty());
    bad = original;
    bad.hexes[bad.robberHex.value].number = 7;
    CHECK(!validateInitialBoard(bad));
    bad = original;
    bad.hexes[0].number = 255;
    CHECK(!validateInitialBoard(bad));
    bad = original;
    bad.ports[1].edge = bad.ports[0].edge;
    CHECK(!validateInitialBoard(bad));
    bad = original;
    for (std::size_t edge = 0; edge < BoardTopology::EdgeCount; ++edge) {
        if (BoardTopology::standard().edges[edge].hexes[1]) {
            bad.ports[0].edge = EdgeId{static_cast<std::uint8_t>(edge)};
            break;
        }
    }
    CHECK(!validateInitialBoard(bad));
    bad = original;
    bad.roads[0] = Road{PlayerId::One};
    CHECK(!validateInitialBoard(bad));
    bool rejected = false;
    try { Game game(bad); } catch (const std::invalid_argument&) { rejected = true; }
    CHECK(rejected);
}
