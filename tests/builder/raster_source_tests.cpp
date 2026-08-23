#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gdal_priv.h>
#include <ogr_spatialref.h>

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
            ("lunar-terrain-m3-" + std::to_string(suffix));
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

struct DatasetDeleter {
    void operator()(GDALDataset* dataset) const noexcept {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetDeleter>;

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
    for (const char value : characters) {
        bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
    }
    return bytes;
}

void set_lunar_georeferencing(GDALDataset& dataset, const int width, const int height) {
    std::array<double, 6> transform{
        -2.0,
        4.0 / static_cast<double>(width),
        0.0,
        2.0,
        0.0,
        -4.0 / static_cast<double>(height),
    };
    REQUIRE(dataset.SetGeoTransform(transform.data()) == CE_None);
    OGRSpatialReference spatial_reference;
    REQUIRE(spatial_reference.SetGeogCS(
        "Lunar test", "D_Moon_test", "Moon_test", 1'737'400.0, 0.0,
        "Reference_Meridian", 0.0, "degree", std::numbers::pi_v<double> / 180.0) == OGRERR_NONE);
    spatial_reference.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    REQUIRE(dataset.SetSpatialRef(&spatial_reference) == CE_None);
}

void write_integer_raster(
    const std::filesystem::path& path,
    const int width,
    const int height,
    const bool center_no_data) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    REQUIRE(driver != nullptr);
    DatasetPtr dataset{driver->Create(path.string().c_str(), width, height, 1, GDT_Int16, nullptr)};
    REQUIRE(dataset != nullptr);
    set_lunar_georeferencing(*dataset, width, height);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    REQUIRE(band != nullptr);
    REQUIRE(band->SetNoDataValue(-32'768.0) == CE_None);
    REQUIRE(band->SetScale(0.5) == CE_None);
    REQUIRE(band->SetOffset(0.0) == CE_None);
    std::vector<std::int16_t> values(static_cast<std::size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            values[static_cast<std::size_t>(y) * width + x] =
                static_cast<std::int16_t>(x + 2 * y - (width * 3 / 2));
        }
    }
    if (center_no_data) {
        values[static_cast<std::size_t>(height / 2 - 1) * width + (width / 2 - 1)] = -32'768;
    }
    REQUIRE(band->RasterIO(
        GF_Write, 0, 0, width, height, values.data(), width, height,
        GDT_Int16, 0, 0, nullptr) == CE_None);
}

void write_radius_raster_without_metadata(
    const std::filesystem::path& path,
    const int width,
    const int height) {
    GDALAllRegister();
    GDALDriver* driver = GetGDALDriverManager()->GetDriverByName("GTiff");
    REQUIRE(driver != nullptr);
    DatasetPtr dataset{driver->Create(path.string().c_str(), width, height, 1, GDT_Float64, nullptr)};
    REQUIRE(dataset != nullptr);
    GDALRasterBand* band = dataset->GetRasterBand(1);
    REQUIRE(band != nullptr);
    std::vector<double> values(static_cast<std::size_t>(width) * height, 1'737'650.0);
    REQUIRE(band->RasterIO(
        GF_Write, 0, 0, width, height, values.data(), width, height,
        GDT_Float64, 0, 0, nullptr) == CE_None);
}

[[nodiscard]] std::string path_text(const std::filesystem::path& path) {
    return path.generic_string();
}

[[nodiscard]] std::string raster_configuration_text(
    const std::filesystem::path& source_root,
    const std::filesystem::path& output,
    const std::string_view raster_name,
    const int width,
    const int height,
    const std::string_view data_type,
    const std::string_view representation,
    const std::string_view no_data_policy,
    const bool metadata_override,
    const double no_data = -32'768.0,
    const double sample_scale = 0.5) {
    return "[database]\n"
           "name = \"GeneratedRaster\"\n"
           "output_directory = \"" + path_text(output) + "\"\n"
           "\n[tiles]\n"
           "max_level = 8\n"
           "\n[raster]\n"
           "stable_key = \"generated.raster.p1.v1\"\n"
           "source_uri = \"fixture://generated-raster-v1\"\n"
           "product_name = \"Generated raster\"\n"
           "producer = \"LunarTerrainBuilder tests\"\n"
           "product_version = \"1\"\n"
           "original_crs = \"Lunar geographic test CRS\"\n"
           "license = \"Generated test data\"\n"
           "raster_member = \"" + std::string{raster_name} + "\"\n"
           "expected_data_type = \"" + std::string{data_type} + "\"\n"
           "artifact_members = [{ name = \"" + std::string{raster_name} + "\" }]\n"
           "expected_width = " + std::to_string(width) + "\n"
           "expected_height = " + std::to_string(height) + "\n"
           "west_longitude_degrees = -2.0\n"
           "east_longitude_degrees = 2.0\n"
           "south_latitude_degrees = -2.0\n"
           "north_latitude_degrees = 2.0\n"
           "nominal_resolution_meters = 100.0\n"
           "source_no_data = " + std::to_string(no_data) + "\n"
           "sample_scale = " + std::to_string(sample_scale) + "\n"
           "sample_offset = 0.0\n"
           "elevation_representation = \"" + std::string{representation} + "\"\n"
           "source_reference_radius_meters = 1737400.0\n"
           "no_data_policy = \"" + std::string{no_data_policy} + "\"\n"
           "metadata_override = " + std::string{metadata_override ? "true" : "false"} + "\n"
           "priority = 0\n"
           "\n[local]\n"
           "source_root = \"" + path_text(source_root) + "\"\n";
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

TEST_CASE("generated GDAL raster catalogs plans and builds deterministic P1 output") {
    TemporaryDirectory temporary;
    const auto first_root = temporary.path() / "source-a";
    const auto second_root = temporary.path() / "source-b";
    std::filesystem::create_directories(first_root);
    std::filesystem::create_directories(second_root);
    const auto first_raster = first_root / "generated.tif";
    const auto second_raster = second_root / "generated.tif";
    write_integer_raster(first_raster, 1'024, 1'024, false);
    REQUIRE(std::filesystem::copy_file(first_raster, second_raster));

    const auto first_path = temporary.path() / "first.toml";
    const auto second_path = temporary.path() / "second.toml";
    write_text(first_path, raster_configuration_text(
        first_root, temporary.path() / "output-a", "generated.tif",
        1'024, 1'024, "Int16", "elevation_meters", "error", false));
    write_text(second_path, raster_configuration_text(
        second_root, temporary.path() / "output-b", "generated.tif",
        1'024, 1'024, "Int16", "elevation_meters", "error", false));

    auto first = load_configuration(first_path);
    auto second = load_configuration(second_path);
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first.value().source_kind == BuilderSourceKind::raster);
    CHECK(first.value().maximum_level == 8);
    auto first_identity = identify_configuration(first.value());
    auto second_identity = identify_configuration(second.value());
    REQUIRE(first_identity);
    REQUIRE(second_identity);
    CHECK(first_identity.value().builder_hash == second_identity.value().builder_hash);
    CHECK(first_identity.value().semantic_hash == second_identity.value().semantic_hash);
    CHECK(first_identity.value().canonical_builder_json.find(path_text(first_root)) == std::string::npos);
    BuilderConfiguration changed_provenance = first.value();
    changed_provenance.raster->producer = "Changed producer";
    auto changed_identity = identify_configuration(changed_provenance);
    REQUIRE(changed_identity);
    CHECK(changed_identity.value().semantic_hash != first_identity.value().semantic_hash);
    CHECK(changed_identity.value().builder_hash != first_identity.value().builder_hash);

    auto source = open_raster_source(first.value(), first_identity.value());
    REQUIRE(source);
    CHECK(source.value()->details().driver_name == "GTiff");
    CHECK(source.value()->details().data_type_name == "Int16");
    CHECK(source.value()->details().width == 1'024);
    CHECK(source.value()->Covers(LunarGeodeticCoordinate{}));
    auto center = source.value()->Sample(LunarGeodeticCoordinate{});
    REQUIRE(center);
    CHECK(center.value().elevation_meters == -0.75);

    auto scan = scan_configuration(first.value());
    REQUIRE(scan);
    REQUIRE(scan.value().raster_details);
    CHECK(scan.value().artifact_members.size() == 1);
    CHECK(scan.value().artifact_bundle_bytes == std::filesystem::file_size(first_raster));
    CHECK(scan.value().center_elevation_meters == -0.75);

    auto plan = plan_configuration(first.value());
    REQUIRE(plan);
    REQUIRE(plan.value().tiles.size() == 1);
    CHECK(plan.value().tiles.front().level() == 8);
    CHECK(plan.value().tiles.front().face() == 0);

    auto first_build = build_configuration(first.value());
    auto second_build = build_configuration(second.value());
    REQUIRE(first_build);
    REQUIRE(second_build);
    CHECK(first_build.value().tile_count == 1);
    CHECK(first_build.value().database_content_hash == second_build.value().database_content_hash);
    CHECK(read_bytes(first_build.value().database_path) == read_bytes(second_build.value().database_path));
    REQUIRE(first_build.value().packs.size() == 1);
    REQUIRE(second_build.value().packs.size() == 1);
    CHECK(read_bytes(first_build.value().packs.front().path) ==
          read_bytes(second_build.value().packs.front().path));

    auto database = LunarTerrainDatabase::Open(first_build.value().database_path);
    REQUIRE(database);
    CHECK(database.value().Header().maximum_level == 8);
    CHECK(database.value().Header().dataset_count == 1);
    const LunarTileKey key = plan.value().tiles.front();
    auto tile = database.value().ReadTile(key);
    REQUIRE(tile);
    REQUIRE(tile.value().provenance());
    CHECK(tile.value().provenance()->palette.front().dataset_id == first_identity.value().dataset_id);
    const DecodedChannel* elevation = elevation_channel(tile.value());
    REQUIRE(elevation != nullptr);
    auto u = QscProjection::LatticeCoordinate(key.x(), 128, key.level());
    auto v = QscProjection::LatticeCoordinate(key.y(), 128, key.level());
    REQUIRE(u);
    REQUIRE(v);
    auto coordinate = QscProjection::Inverse(QscCoordinate{
        static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
    REQUIRE(coordinate);
    auto expected = source.value()->Sample(coordinate.value());
    REQUIRE(expected);
    const auto expected_code = static_cast<std::uint16_t>(
        std::nearbyint((expected.value().elevation_meters + 16'384.0) / 0.5));
    constexpr std::size_t center_stored = 129;
    CHECK(read_u16(
        elevation->bytes(),
        center_stored * format_v1::serialized_elevation_samples + center_stored) == expected_code);
}

TEST_CASE("raster no-data policy is explicit and deterministic") {
    TemporaryDirectory temporary;
    const auto source_root = temporary.path() / "source";
    std::filesystem::create_directories(source_root);
    write_integer_raster(source_root / "nodata.tif", 64, 64, true);
    const auto error_path = temporary.path() / "error.toml";
    const auto nearest_path = temporary.path() / "nearest.toml";
    write_text(error_path, raster_configuration_text(
        source_root, temporary.path() / "error-output", "nodata.tif",
        64, 64, "Int16", "elevation_meters", "error", false));
    write_text(nearest_path, raster_configuration_text(
        source_root, temporary.path() / "nearest-output", "nodata.tif",
        64, 64, "Int16", "elevation_meters", "nearest_valid", false));

    auto error_configuration = load_configuration(error_path);
    auto nearest_configuration = load_configuration(nearest_path);
    REQUIRE(error_configuration);
    REQUIRE(nearest_configuration);
    auto error_identity = identify_configuration(error_configuration.value());
    auto nearest_identity = identify_configuration(nearest_configuration.value());
    REQUIRE(error_identity);
    REQUIRE(nearest_identity);
    CHECK(error_identity.value().semantic_hash != nearest_identity.value().semantic_hash);
    auto error_source = open_raster_source(error_configuration.value(), error_identity.value());
    auto nearest_source = open_raster_source(nearest_configuration.value(), nearest_identity.value());
    REQUIRE(error_source);
    REQUIRE(nearest_source);
    auto rejected = error_source.value()->Sample(LunarGeodeticCoordinate{});
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == ErrorCode::invalid_argument);
    auto filled = nearest_source.value()->Sample(LunarGeodeticCoordinate{});
    REQUIRE(filled);
    CHECK(filled.value().filled_no_data);
}

TEST_CASE("radius rasters require an explicit metadata override and datum normalization") {
    TemporaryDirectory temporary;
    const auto source_root = temporary.path() / "source";
    std::filesystem::create_directories(source_root);
    write_radius_raster_without_metadata(source_root / "radius.tif", 64, 64);
    const auto path = temporary.path() / "radius.toml";
    write_text(path, raster_configuration_text(
        source_root, temporary.path() / "output", "radius.tif",
        64, 64, "Float64", "radius_meters", "error", true, -9'999.0, 1.0));
    auto configuration = load_configuration(path);
    REQUIRE(configuration);
    auto identity = identify_configuration(configuration.value());
    REQUIRE(identity);
    auto source = open_raster_source(configuration.value(), identity.value());
    REQUIRE(source);
    CHECK(source.value()->metadata().flags == 0x00000003U);
    auto sample = source.value()->Sample(LunarGeodeticCoordinate{});
    REQUIRE(sample);
    CHECK(sample.value().elevation_meters == 250.0);
}

}  // namespace
}  // namespace lunar::terrain::builder
