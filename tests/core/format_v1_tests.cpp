#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>
#include <utility>
#include <vector>

#include <lunar/terrain/format_v1.hpp>

namespace lunar::terrain::format_v1 {
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

TEST_CASE("v1 record sizes reconcile with reviewed golden vectors") {
    const std::array fixtures{
        std::pair{"ltdb_header_v1.bin", bytes::ltdb_header},
        std::pair{"chunk_directory_entry_v1.bin", bytes::chunk_directory_entry},
        std::pair{"dataset_record_v1.bin", bytes::dataset_record},
        std::pair{"pack_record_v1.bin", bytes::pack_record},
        std::pair{"tile_index_record_v1.bin", bytes::tile_index_record},
        std::pair{"pack_header_v1.bin", bytes::pack_header},
        std::pair{"tile_header_v1.bin", bytes::tile_header},
        std::pair{"channel_record_elev_v1.bin", bytes::channel_record},
        std::pair{"channel_record_prvn_v1.bin", bytes::channel_record},
        std::pair{"channel_record_qual_v1.bin", bytes::channel_record},
        std::pair{"provenance_header_v1.bin", bytes::provenance_header},
        std::pair{"provenance_palette_entry_v1.bin", bytes::provenance_palette_entry},
    };

    for (const auto& [name, expected_size] : fixtures) {
        CAPTURE(name);
        CHECK(read_fixture(name).size() == expected_size);
    }
}

TEST_CASE("golden header fields occupy their frozen offsets") {
    const auto header = read_fixture("ltdb_header_v1.bin");
    REQUIRE(header.size() == bytes::ltdb_header);

    CHECK(header[ltdb_header_offset::magic + 0] == std::byte{'L'});
    CHECK(header[ltdb_header_offset::magic + 1] == std::byte{'T'});
    CHECK(header[ltdb_header_offset::magic + 2] == std::byte{'D'});
    CHECK(header[ltdb_header_offset::magic + 3] == std::byte{'B'});
    CHECK(header[ltdb_header_offset::major] == std::byte{1});
    CHECK(header[ltdb_header_offset::minor] == std::byte{0});
    CHECK(header[ltdb_header_offset::endian + 0] == std::byte{4});
    CHECK(header[ltdb_header_offset::endian + 1] == std::byte{3});
    CHECK(header[ltdb_header_offset::endian + 2] == std::byte{2});
    CHECK(header[ltdb_header_offset::endian + 3] == std::byte{1});

    for (std::size_t index = ltdb_header_offset::reserved; index < header.size(); ++index) {
        CHECK(header[index] == std::byte{0});
    }
}

TEST_CASE("Delta2D golden channel locks modulo U16 residual order") {
    const auto residuals = read_fixture("elev_channel_logical_v1.bin");
    const std::array<std::uint16_t, 9> expected{100, 1, 2, 0xFFFF, 1, 1, 0xFFFF, 2, 1};
    REQUIRE(residuals.size() == expected.size() * 2);
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto low = std::to_integer<std::uint16_t>(residuals[index * 2]);
        const auto high = std::to_integer<std::uint16_t>(residuals[index * 2 + 1]);
        CHECK(static_cast<std::uint16_t>(low | (high << 8)) == expected[index]);
    }
}

TEST_CASE("complete structural fixtures are present") {
    CHECK(read_fixture("tile_payload_v1.bin").size() == 332);
    CHECK(read_fixture("pack_file_v1.bin").size() == 396);
    CHECK(read_fixture("database_file_v1.bin").size() == 1344);
    CHECK(read_fixture("prvn_channel_v1.bin").size() == 52);
    CHECK(read_fixture("qual_channel_v1.bin").size() == 4);
}

}  // namespace
}  // namespace lunar::terrain::format_v1
