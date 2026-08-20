#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <functional>
#include <string_view>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {
namespace {

TEST_CASE("TileKey validates faces levels and coordinates") {
    CHECK(LunarTileKey::create(0, 0, 0, 0));
    CHECK(LunarTileKey::create(5, 28, (std::uint32_t{1} << 28) - 1, (std::uint32_t{1} << 28) - 1));

    const auto bad_face = LunarTileKey::create(6, 0, 0, 0);
    REQUIRE_FALSE(bad_face);
    CHECK(bad_face.error().code == ErrorCode::invalid_tile_key);
    CHECK_FALSE(LunarTileKey::create(262, 0, 0, 0));

    const auto bad_level = LunarTileKey::create(0, 29, 0, 0);
    REQUIRE_FALSE(bad_level);
    CHECK(bad_level.error().code == ErrorCode::invalid_tile_key);
    CHECK_FALSE(LunarTileKey::create(0, 256, 0, 0));

    CHECK_FALSE(LunarTileKey::create(0, 0, 1, 0));
    CHECK_FALSE(LunarTileKey::create(0, 8, 256, 0));
    CHECK_FALSE(LunarTileKey::create(0, 8, 0, 256));
}

TEST_CASE("TileKey Morton bit order and encoded validation are stable") {
    auto key = LunarTileKey::create(2, 3, 5, 3);
    REQUIRE(key);
    CHECK(key.value().morton() == 27);
    CHECK(key.value().x() == 5);
    CHECK(key.value().y() == 3);
    CHECK(key.value().face() == 2);
    CHECK(key.value().level() == 3);

    auto decoded = LunarTileKey::from_encoded(key.value().encoded());
    REQUIRE(decoded);
    CHECK(decoded.value() == key.value());

    const std::uint64_t unused_bit = std::uint64_t{1} << 20;
    auto invalid = LunarTileKey::from_encoded(key.value().encoded() | unused_bit);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_tile_key);
}

TEST_CASE("TileKey canonical text form parses and formats") {
    auto key = LunarTileKey::parse("QSC/F2/L08/0147/0092");
    REQUIRE(key);
    CHECK(key.value().face() == 2);
    CHECK(key.value().level() == 8);
    CHECK(key.value().x() == 147);
    CHECK(key.value().y() == 92);
    CHECK(key.value().to_string() == "QSC/F2/L08/0147/0092");

    CHECK_FALSE(LunarTileKey::parse("QSC/F2/L8/0147/0092"));
    CHECK_FALSE(LunarTileKey::parse("QSC/F2/L08/147/0092"));
    CHECK_FALSE(LunarTileKey::parse("qsc/F2/L08/0147/0092"));
    CHECK_FALSE(LunarTileKey::parse("QSC/F2/L08/0147/0092/extra"));
}

TEST_CASE("TileKey parent and children preserve quadrant order") {
    auto parent = LunarTileKey::create(4, 7, 11, 29);
    REQUIRE(parent);
    auto children = parent.value().children();
    REQUIRE(children);

    const std::array<std::array<std::uint32_t, 2>, 4> coordinates{
        std::array<std::uint32_t, 2>{22, 58},
        std::array<std::uint32_t, 2>{23, 58},
        std::array<std::uint32_t, 2>{22, 59},
        std::array<std::uint32_t, 2>{23, 59},
    };
    for (std::size_t quadrant = 0; quadrant < children.value().size(); ++quadrant) {
        const auto child = children.value()[quadrant];
        CHECK(child.x() == coordinates[quadrant][0]);
        CHECK(child.y() == coordinates[quadrant][1]);
        REQUIRE(child.parent());
        CHECK(*child.parent() == parent.value());
    }

    auto root = LunarTileKey::create(0, 0, 0, 0);
    REQUIRE(root);
    CHECK_FALSE(root.value().parent());

    auto leaf = LunarTileKey::create(0, 28, 0, 0);
    REQUIRE(leaf);
    auto impossible_children = leaf.value().children();
    REQUIRE_FALSE(impossible_children);
    CHECK(impossible_children.error().code == ErrorCode::invalid_tile_key);
}

TEST_CASE("TileKey ordering and hashing use the stable encoded value") {
    auto first = LunarTileKey::create(0, 1, 0, 0);
    auto second = LunarTileKey::create(0, 1, 1, 0);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.value() < second.value());
    CHECK(std::hash<LunarTileKey>{}(first.value()) ==
          std::hash<std::uint64_t>{}(first.value().encoded()));
}

}  // namespace
}  // namespace lunar::terrain
