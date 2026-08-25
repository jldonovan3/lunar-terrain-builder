#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_projection.hpp>

#include "builder/builder.hpp"
#include "builder/raster_source.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("lunar-terrain-sldem2015-" + std::to_string(suffix));
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

[[nodiscard]] std::optional<std::string> environment_value(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t bytes = 0;
    if (_dupenv_s(&value, &bytes, name) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    const std::vector<char> characters{
        std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes;
    bytes.reserve(characters.size());
    for (const char value : characters) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

[[nodiscard]] const DecodedChannel* elevation_channel(const DecodedTerrainTile& tile) {
    const auto found = std::ranges::find_if(tile.channels(), [](const DecodedChannel& channel) {
        return channel.id() == ChannelId::elevation;
    });
    return found == tile.channels().end() ? nullptr : &*found;
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t index) {
    const std::size_t offset = index * 2U;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

TEST_CASE("the pinned SLDEM2015 subset builds and reconstructs through Core") {
    const auto source_root = environment_value("SLDEM2015_ROOT");
    if (!source_root || source_root->empty()) {
        SKIP("SLDEM2015_ROOT is unset; the external M3 acceptance bundle is opt-in");
    }

    auto configuration = load_configuration(LUNAR_TERRAIN_SLDEM_CONFIG);
    REQUIRE(configuration);
    auto identity = identify_configuration(configuration.value());
    REQUIRE(identity);
    CHECK(identity.value().dataset_ids.front().value == 2'012'013'082U);

    auto source = open_raster_source(configuration.value(), identity.value());
    REQUIRE(source);
    const RasterSourceDetails& details = source.value()->details();
    CHECK(details.driver_name == "JP2OpenJPEG");
    CHECK(details.data_type_name == "Int16");
    CHECK(details.width == 23'040);
    CHECK(details.height == 15'360);
    CHECK(details.footprint.west_longitude_degrees == 0.0);
    CHECK(details.footprint.east_longitude_degrees == 45.0);
    CHECK(details.footprint.south_latitude_degrees == 0.0);
    CHECK(details.footprint.north_latitude_degrees == 30.0);
    CHECK(details.no_data_value == -32'768.0);
    CHECK(details.sample_scale == 0.5);
    CHECK(details.elevation_representation == ElevationRepresentation::elevation_meters);
    CHECK(source.value()->metadata().artifact_bundle_bytes == 171'872'390ULL);
    CHECK(source.value()->metadata().artifact_bundle_hash.to_hex() ==
          "17810a5b1551a56b865f59c20ae2c78c6aa05112112c557f466e21e19d5b9351");
    REQUIRE(source.value()->metadata().artifact_members.size() == 3);
    CHECK(source.value()->metadata().artifact_members.front().sha256.to_hex() ==
          "8d6e9bb9687bd19dbc9de57c8044932540bb934e5ef557b473660f5056484a73");
    CHECK(source.value()->metadata().artifact_members[1].sha256.to_hex() ==
          "c9c013daa77cfbbd68efb157c51198fda3aed157f0d5ebddaf1a94f9151faa71");
    CHECK(source.value()->metadata().artifact_members[2].sha256.to_hex() ==
          "4d8f5edd72cdc486cbe491741a588dc545b7b72baa4cfb0e34cae7389d6305e0");

    const LunarGeodeticCoordinate source_center{
        15.0 * std::numbers::pi_v<double> / 180.0,
        22.5 * std::numbers::pi_v<double> / 180.0,
        0.0,
    };
    auto center_sample = source.value()->Sample(source_center);
    REQUIRE(center_sample);
    CHECK(center_sample.value().elevation_meters == Catch::Approx(-1'346.0).margin(1.0e-9));

    auto plan = plan_configuration(configuration.value());
    REQUIRE(plan);
    REQUIRE(plan.value().tiles.size() == 1);
    auto expected_key = LunarTileKey::parse("QSC/F0/L08/0197/0179");
    REQUIRE(expected_key);
    CHECK(plan.value().tiles.front() == expected_key.value());

    TemporaryDirectory temporary;
    BuilderConfiguration first = configuration.value();
    BuilderConfiguration second = configuration.value();
    first.output_directory = temporary.path() / "first";
    second.output_directory = temporary.path() / "second";
    auto first_build = build_configuration(first);
    auto second_build = build_configuration(second);
    REQUIRE(first_build);
    REQUIRE(second_build);
    CHECK(first_build.value().database_content_hash == second_build.value().database_content_hash);
    CHECK(read_bytes(first_build.value().database_path) == read_bytes(second_build.value().database_path));
    REQUIRE(first_build.value().packs.size() == 1);
    REQUIRE(second_build.value().packs.size() == 1);
    CHECK(read_bytes(first_build.value().packs.front().path) ==
          read_bytes(second_build.value().packs.front().path));

    auto validation = validate_database(first_build.value().database_path, true);
    REQUIRE(validation);
    CHECK(validation.value().tile_count == 1);
    CHECK(validation.value().pack_count == 1);
    CHECK(validation.value().verified_seams == 0);

    auto database = LunarTerrainDatabase::Open(first_build.value().database_path);
    REQUIRE(database);
    CHECK(database.value().Header().maximum_level == 8);
    CHECK(database.value().Header().dataset_count == 1);
    auto tile = database.value().ReadTile(expected_key.value());
    REQUIRE(tile);
    REQUIRE(tile.value().provenance());
    REQUIRE(tile.value().provenance()->palette.size() == 1);
    CHECK(tile.value().provenance()->palette.front().dataset_id ==
          identity.value().dataset_ids.front());
    const DecodedChannel* elevation = elevation_channel(tile.value());
    REQUIRE(elevation != nullptr);

    auto u = QscProjection::LatticeCoordinate(expected_key.value().x(), 128, 8);
    auto v = QscProjection::LatticeCoordinate(expected_key.value().y(), 128, 8);
    REQUIRE(u);
    REQUIRE(v);
    auto coordinate = QscProjection::Inverse(QscCoordinate{QscFace::front, u.value(), v.value(), 0.0});
    REQUIRE(coordinate);
    auto measured = source.value()->Sample(coordinate.value());
    REQUIRE(measured);
    constexpr std::size_t center_stored = 129;
    const std::uint16_t code = read_u16(
        elevation->bytes(),
        center_stored * format_v1::serialized_elevation_samples + center_stored);
    const double reconstructed = -16'384.0 + static_cast<double>(code) * 0.5;
    CHECK(reconstructed == Catch::Approx(measured.value().elevation_meters).margin(0.25));
    CHECK(code >= database.value().FindTile(expected_key.value())->minimum_elevation_code);
    CHECK(code <= database.value().FindTile(expected_key.value())->maximum_elevation_code);
}

}  // namespace
}  // namespace lunar::terrain::builder
