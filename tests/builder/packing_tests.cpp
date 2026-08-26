#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/packing.hpp"

namespace lunar::terrain::builder {
namespace {

TEST_CASE("canonical packing groups Face Level and rolls over deterministically") {
    const auto face0 = LunarTileKey::create(0, 2, 0, 0).value();
    const auto face0_children = face0.children().value();
    const auto face1 = LunarTileKey::create(1, 3, 0, 0).value();
    const std::vector tiles{
        PackingTile{face0_children[0], 100},
        PackingTile{face0_children[1], 100},
        PackingTile{face0_children[2], 100},
        PackingTile{face1, 10},
    };
    // 64-byte header + 100 + 4 bytes alignment + 100 = 268 bytes.
    auto packed = plan_canonical_pack_ranges(tiles, 267);
    REQUIRE(packed);
    REQUIRE(packed.value().size() == 4);
    CHECK(packed.value()[0].first_tile == 0);
    CHECK(packed.value()[0].tile_count == 1);
    CHECK(packed.value()[3].first_tile == 3);

    auto paired = plan_canonical_pack_ranges(tiles, 268);
    REQUIRE(paired);
    REQUIRE(paired.value().size() == 3);
    CHECK(paired.value()[0].tile_count == 2);
    CHECK(paired.value()[1].tile_count == 1);
    CHECK(paired.value()[2].tile_count == 1);
}

TEST_CASE("canonical packing rejects duplicate or unsorted TileKeys") {
    const auto key = LunarTileKey::create(0, 1, 0, 0).value();
    auto duplicate = plan_canonical_pack_ranges(
        std::vector{PackingTile{key, 1}, PackingTile{key, 1}}, 1'024);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == ErrorCode::invalid_argument);
}

}  // namespace
}  // namespace lunar::terrain::builder
