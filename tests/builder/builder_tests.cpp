#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stop_token>
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
    CHECK(version_string() == "0.3.0");
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
    CHECK(first_identity.value().dataset_ids == second_identity.value().dataset_ids);
    CHECK(first_identity.value().canonical_builder_json.find("output-a") == std::string::npos);
    CHECK(first_identity.value().canonical_builder_json.find("cache-a") == std::string::npos);
    CHECK(first_identity.value().canonical_semantic_json.find(
              "lowest_tile_key_patches_v1") != std::string::npos);
    CHECK(first_identity.value().canonical_semantic_json.find(
              "quantized_neighbor_or_virtual_v1") != std::string::npos);
    CHECK(first_identity.value().builder_hash != changed_identity.value().builder_hash);
    CHECK(first_identity.value().semantic_hash != changed_identity.value().semantic_hash);

    const auto invalid_path = temporary.path() / "invalid.toml";
    write_text(invalid_path, "[database]\nname = \"Moon\"\ntimestmp = true\n");
    auto invalid = load_configuration(invalid_path);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == ErrorCode::invalid_argument);
    CHECK(invalid.error().message.find("unknown configuration key") != std::string::npos);
}

TEST_CASE("M8 logical raster bundles and required regions are semantic") {
    TemporaryDirectory temporary;
    const auto make_configuration = [](const std::string_view source_root) {
        return std::string{R"toml([database]
name = "M8Parser"

[tiles]
max_level = 12

[region]
west_longitude_degrees = 10.0
east_longitude_degrees = 11.0
south_latitude_degrees = -2.0
north_latitude_degrees = 2.0

[raster]
stable_key = "example.m8.mosaic.v1"
source_uri = "https://example.test/m8/"
original_crs = "test lunar geographic"
expected_data_type = "Int16"
artifact_bundle_bytes = 2
artifact_bundle_sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
artifact_members = [
  { name = "a.tif", bytes = 1, sha256 = "0000000000000000000000000000000000000000000000000000000000000000" },
  { name = "b.tif", bytes = 1, sha256 = "1111111111111111111111111111111111111111111111111111111111111111" },
]
raster_files = [
  { member = "b.tif", expected_width = 8, expected_height = 8, west_longitude_degrees = 11.0, east_longitude_degrees = 12.0, south_latitude_degrees = -2.0, north_latitude_degrees = 2.0 },
  { member = "a.tif", expected_width = 8, expected_height = 8, west_longitude_degrees = 10.0, east_longitude_degrees = 11.0, south_latitude_degrees = -2.0, north_latitude_degrees = 2.0 },
]
nominal_resolution_meters = 20.0
effective_resolution_meters = 80.0
source_no_data = nan
sample_scale = 1.0
elevation_representation = "elevation_meters"
quality_mapping = "b.tif supplies lower-confidence evidence"
quality_members = ["b.tif"]
unsupported_quality_values = ["exact confidence magnitude"]
source_root = ")toml"} + std::string{source_root} + R"toml("
)toml";
    };
    const auto first_path = temporary.path() / "m8-first.toml";
    const auto second_path = temporary.path() / "m8-second.toml";
    write_text(first_path, make_configuration("source-a"));
    write_text(second_path, make_configuration("source-b"));

    auto first = load_configuration(first_path);
    auto second = load_configuration(second_path);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE(first.value().required_region);
    REQUIRE(first.value().rasters.size() == 1);
    const RasterConfiguration& raster = first.value().rasters.front();
    REQUIRE(raster.raster_files.size() == 2);
    CHECK(raster.raster_files.front().member == "a.tif");
    CHECK(std::isnan(raster.source_no_data));
    CHECK(raster.quality_members == std::vector<std::string>{"b.tif"});

    auto first_identity = identify_configuration(first.value());
    auto second_identity = identify_configuration(second.value());
    REQUIRE(first_identity);
    REQUIRE(second_identity);
    CHECK(first_identity.value().semantic_hash == second_identity.value().semantic_hash);
    CHECK(first_identity.value().canonical_semantic_json.find("required_region_degrees") !=
          std::string::npos);
    CHECK(first_identity.value().canonical_semantic_json.find("raster_files") !=
          std::string::npos);
    CHECK(first_identity.value().canonical_semantic_json.find("\"source_no_data\":\"nan\"") !=
          std::string::npos);
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

TEST_CASE("incremental synthetic rebuild reuses staged tiles and converges with clean output") {
    TemporaryDirectory temporary;
    BuilderConfiguration first;
    first.output_directory = temporary.path() / "first";
    first.cache_directory = temporary.path() / "shared-cache";
    first.database_name = "MoonSynthetic";
    first.synthetic_stable_key = "synthetic.p0.v1";
    first.synthetic_source_uri = "synthetic://analytic-v1";
    first.worker_threads = 3;

    auto initial = build_synthetic(first, BuildOptions{false, {}});
    REQUIRE(initial);
    CHECK(initial.value().built_tile_count == 6);
    CHECK(initial.value().reused_tile_count == 0);
    CHECK(std::filesystem::is_regular_file(
        first.cache_directory / "cache.sqlite"));

    BuilderConfiguration incremental_configuration = first;
    incremental_configuration.output_directory = temporary.path() / "incremental";
    auto incremental = build_synthetic(
        incremental_configuration, BuildOptions{true, {}});
    REQUIRE(incremental);
    CHECK(incremental.value().built_tile_count == 0);
    CHECK(incremental.value().reused_tile_count == 6);

    BuilderConfiguration clean_configuration = first;
    clean_configuration.output_directory = temporary.path() / "clean";
    clean_configuration.cache_directory = temporary.path() / "clean-cache";
    auto clean = build_synthetic(clean_configuration, BuildOptions{false, {}});
    REQUIRE(clean);
    CHECK(clean.value().built_tile_count == 6);
    CHECK(clean.value().reused_tile_count == 0);

    CHECK(read_bytes(incremental.value().database_path) ==
          read_bytes(clean.value().database_path));
    REQUIRE(incremental.value().packs.size() == clean.value().packs.size());
    for (std::size_t index = 0; index < incremental.value().packs.size(); ++index) {
        CHECK(read_bytes(incremental.value().packs[index].path) ==
              read_bytes(clean.value().packs[index].path));
    }
}

TEST_CASE("cancelled synthetic build publishes no database") {
    TemporaryDirectory temporary;
    BuilderConfiguration configuration;
    configuration.output_directory = temporary.path() / "cancelled";
    configuration.cache_directory = temporary.path() / "cache";
    configuration.database_name = "MoonSynthetic";
    configuration.synthetic_stable_key = "synthetic.p0.v1";
    configuration.synthetic_source_uri = "synthetic://analytic-v1";

    std::stop_source cancellation;
    cancellation.request_stop();
    auto build = build_synthetic(
        configuration, BuildOptions{false, cancellation.get_token()});
    REQUIRE_FALSE(build);
    CHECK(build.error().code == ErrorCode::cancelled);
    CHECK_FALSE(std::filesystem::exists(
        configuration.output_directory / "MoonSynthetic.ltdb"));
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
