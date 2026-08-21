#include <catch2/catch_test_macros.hpp>

#include <zstd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string_view>
#include <vector>

#include <lunar/terrain/byte_io.hpp>
#include <lunar/terrain/codec.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/integrity.hpp>

namespace lunar::terrain {
namespace {

[[nodiscard]] std::vector<std::byte> read_fixture(const std::string_view name) {
    const auto path = std::filesystem::path{LUNAR_TERRAIN_GOLDEN_DIR} / name;
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<char> chars{
        std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(chars.size());
    for (const char value : chars) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

TEST_CASE("checked byte arithmetic reports overflow") {
    auto sum = checked_add(10, 20);
    auto product = checked_multiply(10, 20);
    REQUIRE(sum);
    REQUIRE(product);
    CHECK(sum.value() == 30);
    CHECK(product.value() == 200);

    auto overflow_sum = checked_add(std::numeric_limits<std::uint64_t>::max(), 1);
    auto overflow_product = checked_multiply(std::numeric_limits<std::uint64_t>::max(), 2);
    REQUIRE_FALSE(overflow_sum);
    REQUIRE_FALSE(overflow_product);
    CHECK(overflow_sum.error().code == ErrorCode::arithmetic_overflow);
    CHECK(overflow_product.error().code == ErrorCode::arithmetic_overflow);
}

TEST_CASE("little-endian reader decodes frozen header fields and reports offsets") {
    const auto header = read_fixture("ltdb_header_v1.bin");
    LittleEndianReader reader{header, 1000};
    auto major = reader.read_u16(4);
    auto endian = reader.read_u32(12);
    auto radius = reader.read_f64(48);
    REQUIRE(major);
    REQUIRE(endian);
    REQUIRE(radius);
    CHECK(major.value() == 1);
    CHECK(endian.value() == 0x01020304U);
    CHECK(radius.value() == 1'737'400.0);

    auto truncated = reader.read_u64(header.size() - 4U);
    REQUIRE_FALSE(truncated);
    CHECK(truncated.error().code == ErrorCode::truncated_data);
    CHECK(truncated.error().context.file_offset == 1000U + header.size() - 4U);

    auto overflow = reader.read_bytes(std::numeric_limits<std::uint64_t>::max(), 2);
    REQUIRE_FALSE(overflow);
    CHECK(overflow.error().code == ErrorCode::arithmetic_overflow);
}

TEST_CASE("CRC32C and SHA-256 match standard deterministic vectors") {
    constexpr std::string_view crc_text = "123456789";
    const auto crc_bytes = std::as_bytes(std::span{crc_text});
    CHECK(crc32c(crc_bytes) == 0xE3069283U);

    constexpr std::string_view sha_text = "abc";
    auto digest = sha256(std::as_bytes(std::span{sha_text}));
    REQUIRE(digest);
    CHECK(digest.value().to_hex() ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("Delta2D reversal reconstructs the reviewed modulo-U16 channel") {
    const auto residuals = read_fixture("elev_channel_logical_v1.bin");
    const auto expected = read_fixture("elev_channel_decoded_v1.bin");
    auto decoded = reverse_delta2d_u16(residuals, 3, 3);
    REQUIRE(decoded);
    CHECK(decoded.value() == expected);

    auto bad_shape = reverse_delta2d_u16(residuals, 2, 3);
    REQUIRE_FALSE(bad_shape);
    CHECK(bad_shape.error().code == ErrorCode::invalid_format);
}

TEST_CASE("Zstandard decompression enforces content size and detects corruption") {
    const std::array source{
        std::byte{0}, std::byte{1}, std::byte{2}, std::byte{3},
        std::byte{3}, std::byte{2}, std::byte{1}, std::byte{0},
    };
    std::vector<std::byte> compressed(ZSTD_compressBound(source.size()));
    const std::size_t compressed_size =
        ZSTD_compress(compressed.data(), compressed.size(), source.data(), source.size(), 3);
    REQUIRE(ZSTD_isError(compressed_size) == 0);
    compressed.resize(compressed_size);

    auto decoded = decompress_zstandard(compressed, source.size());
    REQUIRE(decoded);
    CHECK(std::equal(decoded.value().begin(), decoded.value().end(), source.begin(), source.end()));

    auto wrong_size = decompress_zstandard(compressed, source.size() + 1U);
    REQUIRE_FALSE(wrong_size);
    CHECK(wrong_size.error().code == ErrorCode::invalid_format);

    compressed[0] ^= std::byte{0xFF};
    auto corrupt = decompress_zstandard(compressed, source.size());
    REQUIRE_FALSE(corrupt);
    CHECK(corrupt.error().code == ErrorCode::decompression_failed);
}

}  // namespace
}  // namespace lunar::terrain
