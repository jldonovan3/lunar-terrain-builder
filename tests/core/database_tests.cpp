#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/tile_key.hpp>

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

void write_bytes(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(stream.good());
}

class GoldenDatabaseFiles {
public:
    GoldenDatabaseFiles() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        root_ = std::filesystem::temp_directory_path() /
                ("lunar-terrain-m1-" + std::to_string(suffix));
        database_path_ = root_ / "Fixture.ltdb";
        pack_path_ = root_ / "Packs" / "Fixture_F2_L08_P0000.ltp";
        write_bytes(database_path_, read_fixture("database_file_v1.bin"));
        write_bytes(pack_path_, read_fixture("pack_file_v1.bin"));
    }

    ~GoldenDatabaseFiles() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    [[nodiscard]] const std::filesystem::path& database_path() const noexcept {
        return database_path_;
    }

    [[nodiscard]] const std::filesystem::path& pack_path() const noexcept { return pack_path_; }

private:
    std::filesystem::path root_;
    std::filesystem::path database_path_;
    std::filesystem::path pack_path_;
};

static_assert(!std::is_copy_constructible_v<LunarTerrainDatabase>);
static_assert(!std::is_copy_assignable_v<LunarTerrainDatabase>);
static_assert(std::is_nothrow_move_constructible_v<LunarTerrainDatabase>);
static_assert(std::is_nothrow_move_assignable_v<LunarTerrainDatabase>);

TEST_CASE("database opens the complete golden manifest and serves sparse index queries") {
    GoldenDatabaseFiles files;
    auto database = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE(database);
    CHECK(database.value().Header().major_version == 1);
    CHECK(database.value().Header().minor_version == 0);
    CHECK(database.value().Header().tile_count == 1);
    CHECK(database.value().Header().dataset_count == 1);
    CHECK(database.value().Header().pack_count == 1);

    auto key = LunarTileKey::parse("QSC/F2/L08/0147/0092");
    REQUIRE(key);
    const auto entry = database.value().FindTile(key.value());
    REQUIRE(entry);
    CHECK(entry->key == key.value());
    CHECK(entry->pack_id.value == 0);
    CHECK(entry->channel_count == 3);

    const auto subtree = database.value().QuerySubtree(key.value());
    REQUIRE(subtree.size() == 1);
    CHECK(subtree.front().key == key.value());
    CHECK(database.value().Children(key.value()).empty());

    auto missing_key = LunarTileKey::create(2, 8, 146, 92);
    REQUIRE(missing_key);
    CHECK_FALSE(database.value().FindTile(missing_key.value()));
    auto missing_tile = database.value().ReadTile(missing_key.value());
    REQUIRE_FALSE(missing_tile);
    CHECK(missing_tile.error().code == ErrorCode::not_found);
    CHECK(missing_tile.error().context.tile_key == missing_key.value().encoded());
}

TEST_CASE("database reads the golden pack into owning decoded tile data") {
    GoldenDatabaseFiles files;
    auto database = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE(database);
    auto key = LunarTileKey::parse("QSC/F2/L08/0147/0092");
    REQUIRE(key);

    auto tile = database.value().ReadTile(key.value());
    REQUIRE(tile);
    CHECK(tile.value().key() == key.value());
    CHECK(tile.value().metadata().effective_resolution_meters == 53.0F);
    CHECK(tile.value().metadata().geometric_error_meters == 1.25F);
    REQUIRE(tile.value().channels().size() == 3);

    const auto elevation = std::ranges::find_if(tile.value().channels(), [](const DecodedChannel& channel) {
        return channel.id() == ChannelId::elevation;
    });
    REQUIRE(elevation != tile.value().channels().end());
    CHECK(std::ranges::equal(elevation->bytes(), read_fixture("elev_channel_decoded_v1.bin")));

    REQUIRE(tile.value().provenance());
    CHECK(tile.value().provenance()->palette.size() == 2);
    CHECK(tile.value().provenance()->map_width == 2);
    CHECK(tile.value().provenance()->map_height == 2);
    CHECK(tile.value().provenance()->dominant_source_indices ==
          std::vector<std::uint16_t>{0, 0, 1, 1});
}

TEST_CASE("database rejects header, version, truncation, and chunk corruption") {
    GoldenDatabaseFiles files;
    auto original = read_fixture("database_file_v1.bin");

    auto bad_magic = original;
    bad_magic[0] = std::byte{'X'};
    write_bytes(files.database_path(), bad_magic);
    auto magic_result = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE_FALSE(magic_result);
    CHECK(magic_result.error().code == ErrorCode::invalid_format);
    CHECK(magic_result.error().context.file_offset == 0);

    auto bad_version = original;
    bad_version[4] = std::byte{2};
    write_bytes(files.database_path(), bad_version);
    auto version_result = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE_FALSE(version_result);
    CHECK(version_result.error().code == ErrorCode::unsupported_version);

    auto truncated = original;
    truncated.resize(100);
    write_bytes(files.database_path(), truncated);
    auto truncated_result = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE_FALSE(truncated_result);
    CHECK(truncated_result.error().code == ErrorCode::truncated_data);

    auto bad_chunk = original;
    bad_chunk.back() ^= std::byte{1};
    write_bytes(files.database_path(), bad_chunk);
    auto chunk_result = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE_FALSE(chunk_result);
    CHECK(chunk_result.error().code == ErrorCode::checksum_mismatch);
}

TEST_CASE("database detects corruption in a referenced pack") {
    GoldenDatabaseFiles files;
    auto database = LunarTerrainDatabase::Open(files.database_path());
    REQUIRE(database);

    auto corrupt_pack = read_fixture("pack_file_v1.bin");
    corrupt_pack.back() ^= std::byte{1};
    write_bytes(files.pack_path(), corrupt_pack);

    auto key = LunarTileKey::parse("QSC/F2/L08/0147/0092");
    REQUIRE(key);
    auto tile = database.value().ReadTile(key.value());
    REQUIRE_FALSE(tile);
    CHECK(tile.error().code == ErrorCode::hash_mismatch);
    CHECK(std::filesystem::equivalent(
        std::filesystem::path{tile.error().context.path}, files.pack_path()));
}

}  // namespace
}  // namespace lunar::terrain
