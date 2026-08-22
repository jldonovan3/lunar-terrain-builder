#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>

#include "builder/builder.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("lunar-terrain-m2-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

void write_text(const std::filesystem::path& path, const std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    REQUIRE(stream.is_open());
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(stream.good());
}

[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<char> characters{
        std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(characters.size());
    for (const char character : characters) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    return bytes;
}

[[nodiscard]] std::string configuration_text(
    const std::string_view output,
    const std::string_view cache,
    const std::uint32_t threads,
    const std::int32_t amplitude = 2'048) {
    return "[database]\n"
           "name = \"MoonSynthetic\"\n"
           "output_directory = \"" + std::string{output} + "\"\n"
           "\n[packaging]\n"
           "target_pack_bytes = 1073741824\n"
           "codec = \"zstd\"\n"
           "codec_level = 3\n"
           "\n[synthetic]\n"
           "stable_key = \"synthetic.p0.v1\"\n"
           "source_uri = \"synthetic://analytic-v1\"\n"
           "amplitude_meters = " + std::to_string(amplitude) + "\n"
           "\n[local]\n"
           "threads = " + std::to_string(threads) + "\n"
           "cache_directory = \"" + std::string{cache} + "\"\n";
}

[[nodiscard]] const DecodedChannel* find_elevation(const DecodedTerrainTile& tile) {
    const auto found = std::ranges::find_if(tile.channels(), [](const DecodedChannel& channel) {
        return channel.id() == ChannelId::elevation;
    });
    return found == tile.channels().end() ? nullptr : &*found;
}

TEST_CASE("builder library exposes its deterministic version") {
    CHECK(version_string() == "0.2.0");
}

TEST_CASE("typed configuration excludes local execution settings from identity") {
    TemporaryDirectory temporary;
    const auto first_path = temporary.path() / "first.toml";
    const auto second_path = temporary.path() / "second.toml";
    const auto changed_path = temporary.path() / "changed.toml";
    write_text(first_path, configuration_text("output-a", "cache-a", 1));
    write_text(second_path, configuration_text("output-b", "cache-b", 8));
    write_text(changed_path, configuration_text("output-c", "cache-c", 2, 2'049));

    auto first = load_configuration(first_path);
    auto second = load_configuration(second_path);
    auto changed = load_configuration(changed_path);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(changed);
    CHECK(first.value().output_directory == temporary.path() / "output-a");
    CHECK(second.value().worker_threads == 8);

    auto first_identity = identify_configuration(first.value());
    auto second_identity = identify_configuration(second.value());
    auto changed_identity = identify_configuration(changed.value());
    REQUIRE(first_identity);
    REQUIRE(second_identity);
    REQUIRE(changed_identity);
    CHECK(first_identity.value().builder_hash == second_identity.value().builder_hash);
    CHECK(first_identity.value().semantic_hash == second_identity.value().semantic_hash);
    CHECK(first_identity.value().synthetic_dataset_id == second_identity.value().synthetic_dataset_id);
    CHECK(first_identity.value().canonical_builder_json.find("output-a") == std::string::npos);
    CHECK(first_identity.value().canonical_builder_json.find("cache-a") == std::string::npos);
    CHECK(first_identity.value().builder_hash != changed_identity.value().builder_hash);
    CHECK(first_identity.value().semantic_hash != changed_identity.value().semantic_hash);

    const auto invalid_path = temporary.path() / "invalid.toml";
    write_text(invalid_path, "[database]\nname = \"Moon\"\ntimestmp = true\n");
    auto invalid = load_configuration(invalid_path);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_argument);
    CHECK(invalid.error().message.find("unknown configuration key") != std::string::npos);
}

TEST_CASE("synthetic planner emits the six canonical root faces") {
    BuilderConfiguration configuration;
    configuration.database_name = "MoonSynthetic";
    configuration.synthetic_stable_key = "synthetic.p0.v1";
    configuration.synthetic_source_uri = "synthetic://analytic-v1";
    auto report = plan_synthetic(configuration);
    REQUIRE(report);
    REQUIRE(report.value().tiles.size() == 6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        CAPTURE(face);
        CHECK(report.value().tiles[face].face() == face);
        CHECK(report.value().tiles[face].level() == 0);
        CHECK(report.value().tiles[face].x() == 0);
        CHECK(report.value().tiles[face].y() == 0);
    }
    CHECK(report.value().estimated_uncompressed_channel_bytes == 805'164);
}

TEST_CASE("two clean synthetic builds are byte-identical and round-trip without cracks") {
    TemporaryDirectory temporary;
    BuilderConfiguration first;
    first.output_directory = temporary.path() / "first";
    first.cache_directory = temporary.path() / "cache-first";
    first.database_name = "MoonSynthetic";
    first.synthetic_stable_key = "synthetic.p0.v1";
    first.synthetic_source_uri = "synthetic://analytic-v1";
    first.worker_threads = 1;
    BuilderConfiguration second = first;
    second.output_directory = temporary.path() / "second";
    second.cache_directory = temporary.path() / "cache-second";
    second.worker_threads = 8;

    auto first_build = build_synthetic(first);
    auto second_build = build_synthetic(second);
    REQUIRE(first_build);
    REQUIRE(second_build);
    CHECK(first_build.value().tile_count == 6);
    CHECK(first_build.value().packs.size() == 6);
    CHECK(first_build.value().database_content_hash == second_build.value().database_content_hash);
    CHECK(first_build.value().builder_configuration_hash == second_build.value().builder_configuration_hash);
    CHECK(read_bytes(first_build.value().database_path) == read_bytes(second_build.value().database_path));
    REQUIRE(first_build.value().packs.size() == second_build.value().packs.size());
    for (std::size_t index = 0; index < first_build.value().packs.size(); ++index) {
        CHECK(first_build.value().packs[index].sha256 == second_build.value().packs[index].sha256);
        CHECK(read_bytes(first_build.value().packs[index].path) ==
              read_bytes(second_build.value().packs[index].path));
    }

    auto database = LunarTerrainDatabase::Open(first_build.value().database_path);
    REQUIRE(database);
    CHECK(database.value().Header().tile_count == 6);
    CHECK(database.value().Header().dataset_count == 1);
    CHECK(database.value().Header().pack_count == 6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto key = LunarTileKey::create(face, 0, 0, 0);
        REQUIRE(key);
        auto tile = database.value().ReadTile(key.value());
        REQUIRE(tile);
        REQUIRE(tile.value().channels().size() == 2);
        const DecodedChannel* elevation = find_elevation(tile.value());
        REQUIRE(elevation != nullptr);
        CHECK(elevation->width() == format_v1::serialized_elevation_samples);
        CHECK(elevation->height() == format_v1::serialized_elevation_samples);
        CHECK(elevation->bytes().size() ==
              std::size_t{format_v1::serialized_elevation_samples} *
                  format_v1::serialized_elevation_samples * 2U);
        REQUIRE(tile.value().provenance());
        CHECK(tile.value().provenance()->palette.size() == 1);
        CHECK(tile.value().provenance()->dominant_source_indices.empty());
    }

    auto validation = validate_database(first_build.value().database_path, true);
    REQUIRE(validation);
    CHECK(validation.value().verified_seams == 12);
    auto inspection = inspect_database(
        first_build.value().database_path, LunarTileKey::create(4, 0, 0, 0).value());
    REQUIRE(inspection);
    CHECK(inspection.value().tile_key.has_value());
    CHECK(inspection.value().channel_count == 2);
}

TEST_CASE("failed publication leaves the previous database readable") {
    TemporaryDirectory temporary;
    BuilderConfiguration configuration;
    configuration.output_directory = temporary.path() / "published";
    configuration.cache_directory = temporary.path() / "cache";
    configuration.database_name = "MoonSynthetic";
    configuration.synthetic_stable_key = "synthetic.p0.v1";
    configuration.synthetic_source_uri = "synthetic://analytic-v1";

    auto original = build_synthetic(configuration);
    REQUIRE(original);
    const auto original_database_bytes = read_bytes(original.value().database_path);

    configuration.synthetic_amplitude_meters = 2'049;
    const auto blocked_temporary =
        configuration.output_directory / ".MoonSynthetic.ltdb.tmp";
    std::filesystem::create_directories(blocked_temporary);
    auto failed = build_synthetic(configuration);
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == ErrorCode::io_error);
    CHECK(read_bytes(original.value().database_path) == original_database_bytes);

    auto still_valid = validate_database(original.value().database_path, true);
    REQUIRE(still_valid);
    CHECK(still_valid.value().tile_count == 6);
}

}  // namespace
}  // namespace lunar::terrain::builder
