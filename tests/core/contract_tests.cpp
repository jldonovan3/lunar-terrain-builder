#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {
namespace {

TEST_CASE("Result carries values and stable contextual errors") {
    auto value = Result<int>::success(42);
    REQUIRE(value);
    CHECK(value.value() == 42);

    Error error{ErrorCode::checksum_mismatch, "payload CRC32C differs"};
    error.with_path("Moon.ltdb").with_offset(512).with_tile_key(0x1234).with_channel(1);
    auto failure = Result<int>::failure(std::move(error));

    REQUIRE_FALSE(failure);
    CHECK(failure.error().code == ErrorCode::checksum_mismatch);
    CHECK(error_code_name(failure.error().code) == "checksum_mismatch");
    CHECK(failure.error().context.path == "Moon.ltdb");
    CHECK(failure.error().context.file_offset == 512);
    CHECK(failure.error().context.tile_key == 0x1234);
    CHECK(failure.error().context.channel_id == 1);

    CHECK(Result<void>::success());
    auto void_failure = Result<void>::failure(Error{ErrorCode::invalid_format, "bad record"});
    REQUIRE_FALSE(void_failure);
    CHECK(void_failure.error().code == ErrorCode::invalid_format);
}

TEST_CASE("SHA-256 digest text is lowercase and round trips") {
    constexpr auto text =
        "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
    auto parsed = Sha256Digest::from_hex(text);
    REQUIRE(parsed);
    CHECK(parsed.value().to_hex() == text);

    auto uppercase = Sha256Digest::from_hex(
        "000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F");
    REQUIRE(uppercase);
    CHECK(uppercase.value() == parsed.value());

    auto short_value = Sha256Digest::from_hex("00");
    REQUIRE_FALSE(short_value);
    CHECK(short_value.error().code == ErrorCode::parse_error);

    std::string invalid(64, 'z');
    auto invalid_value = Sha256Digest::from_hex(invalid);
    REQUIRE_FALSE(invalid_value);
    CHECK(invalid_value.error().code == ErrorCode::parse_error);
}

TEST_CASE("Decoded channels own their byte storage") {
    auto key = LunarTileKey::create(0, 0, 0, 0);
    REQUIRE(key);

    std::vector<std::byte> source{std::byte{1}, std::byte{2}};
    DecodedChannel channel{ChannelId::quality, 1, ElementType::u8, 1, 2, 1, 0, 0, source};
    source[0] = std::byte{9};

    DecodedTerrainTile tile{
        key.value(),
        DecodedTileMetadata{},
        std::vector<DecodedChannel>{std::move(channel)},
        std::nullopt};
    REQUIRE(tile.channels().size() == 1);
    CHECK(tile.channels()[0].bytes()[0] == std::byte{1});
}

}  // namespace
}  // namespace lunar::terrain
