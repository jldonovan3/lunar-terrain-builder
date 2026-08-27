#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>

#include "builder/builder.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("lunar-terrain-m7-" + std::to_string(suffix));
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

[[nodiscard]] std::vector<char> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE(stream.is_open());
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

[[nodiscard]] BuilderConfiguration synthetic_configuration(
    const std::filesystem::path& root,
    const std::string_view name,
    const std::int32_t amplitude = 2'048) {
    BuilderConfiguration configuration;
    configuration.output_directory = root / (std::string{name} + "-output");
    configuration.cache_directory = root / (std::string{name} + "-cache");
    configuration.database_name = "MoonSynthetic";
    configuration.synthetic_stable_key = "synthetic.p0.v1";
    configuration.synthetic_source_uri = "synthetic://analytic-v1";
    configuration.synthetic_amplitude_meters = amplitude;
    configuration.worker_threads = 2;
    return configuration;
}

TEST_CASE("M7 full validation covers projection scientific provenance hierarchy and seams") {
    TemporaryDirectory temporary;
    const BuilderConfiguration configuration =
        synthetic_configuration(temporary.path(), "validation");
    auto build = build_configuration(configuration);
    REQUIRE(build);

    auto validation = validate_database(build.value().database_path, true);
    REQUIRE(validation);
    CHECK(validation.value().full);
    CHECK(validation.value().verified_projection_samples == 30);
    CHECK(validation.value().verified_scientific_tiles == 6);
    CHECK(validation.value().verified_provenance_tiles == 6);
    CHECK(validation.value().verified_hierarchy_tiles == 6);
    CHECK(validation.value().verified_seams == 12);

    auto key = LunarTileKey::create(0, 0, 0, 0);
    REQUIRE(key);
    auto inspection = inspect_database(build.value().database_path, key.value());
    REQUIRE(inspection);
    CHECK(inspection.value().pack_id == 0);
    CHECK(inspection.value().payload_offset == format_v1::bytes::pack_header);
    CHECK(inspection.value().effective_resolution_millimeters == 10'000'000);
    CHECK(inspection.value().geometric_error_millimeters == 0);
    CHECK(inspection.value().materialized_child_mask == 0);
    CHECK_FALSE(inspection.value().parent);
    CHECK(inspection.value().children.empty());
    CHECK(inspection.value().contributing_datasets.size() == 1);
    CHECK(inspection.value().content_hash_prefix.size() == 32);
    CHECK(inspection.value().dependency_hash_prefix.size() == 16);
}

TEST_CASE("M7 diff classifies dataset dependency content provenance and package changes") {
    TemporaryDirectory temporary;
    BuilderConfiguration baseline = synthetic_configuration(temporary.path(), "baseline");
    BuilderConfiguration amplitude = synthetic_configuration(temporary.path(), "amplitude", 2'049);
    BuilderConfiguration dataset = synthetic_configuration(temporary.path(), "dataset");
    dataset.synthetic_stable_key = "synthetic.p0.revised.v1";

    auto baseline_build = build_configuration(baseline);
    auto amplitude_build = build_configuration(amplitude);
    auto dataset_build = build_configuration(dataset);
    REQUIRE(baseline_build);
    REQUIRE(amplitude_build);
    REQUIRE(dataset_build);

    auto identical = diff_databases(
        baseline_build.value().database_path, baseline_build.value().database_path);
    REQUIRE(identical);
    CHECK(identical.value().identical());

    auto amplitude_diff = diff_databases(
        baseline_build.value().database_path, amplitude_build.value().database_path);
    REQUIRE(amplitude_diff);
    CHECK_FALSE(amplitude_diff.value().identical());
    CHECK(amplitude_diff.value().dataset_registry_changed);
    CHECK(amplitude_diff.value().builder_configuration_changed);
    CHECK(amplitude_diff.value().dependency_changes.size() == 6);
    CHECK(amplitude_diff.value().content_changes.size() == 6);
    CHECK(amplitude_diff.value().provenance_changes.empty());

    auto dataset_diff = diff_databases(
        baseline_build.value().database_path, dataset_build.value().database_path);
    REQUIRE(dataset_diff);
    CHECK(dataset_diff.value().added_datasets.size() == 1);
    CHECK(dataset_diff.value().removed_datasets.size() == 1);
    CHECK(dataset_diff.value().provenance_changes.size() == 6);
}

TEST_CASE("M7 export writes meshes raster samples raw elevation and provenance") {
    TemporaryDirectory temporary;
    const BuilderConfiguration configuration =
        synthetic_configuration(temporary.path(), "exports");
    auto build = build_configuration(configuration);
    REQUIRE(build);
    auto key = LunarTileKey::create(0, 0, 0, 0);
    REQUIRE(key);

    const auto ply_path = temporary.path() / "tile.ply";
    const auto obj_path = temporary.path() / "tile.obj";
    const auto pgm_path = temporary.path() / "tile.pgm";
    const auto csv_path = temporary.path() / "tile.csv";
    const auto raw_path = temporary.path() / "tile.u16";
    const auto provenance_path = temporary.path() / "provenance.ppm";
    REQUIRE(export_tile(build.value().database_path, key.value(), DiagnosticExportFormat::ply, ply_path));
    REQUIRE(export_tile(build.value().database_path, key.value(), DiagnosticExportFormat::obj, obj_path));
    REQUIRE(export_tile(
        build.value().database_path, key.value(), DiagnosticExportFormat::elevation_pgm, pgm_path));
    REQUIRE(export_tile(
        build.value().database_path, key.value(), DiagnosticExportFormat::sample_csv, csv_path));
    REQUIRE(export_tile(
        build.value().database_path, key.value(), DiagnosticExportFormat::raw_u16_le, raw_path));
    REQUIRE(export_tile(
        build.value().database_path,
        key.value(),
        DiagnosticExportFormat::provenance_ppm,
        provenance_path));

    const auto ply = read_bytes(ply_path);
    const auto obj = read_bytes(obj_path);
    const auto pgm = read_bytes(pgm_path);
    const auto csv = read_bytes(csv_path);
    CHECK(std::string_view{ply.data(), 3} == "ply");
    CHECK(std::string_view{obj.data(), 1} == "#");
    CHECK(std::string_view{pgm.data(), 2} == "P5");
    CHECK(std::string_view{csv.data(), 8} == "sample_x");
    CHECK(std::filesystem::file_size(raw_path) ==
          std::uint64_t{format_v1::core_vertices} * format_v1::core_vertices * 2U);
    CHECK(std::filesystem::file_size(provenance_path) == 13U + 64U * 64U * 3U);
}

TEST_CASE("M7 benchmark records the scale-gate metrics and deterministic reuse") {
    TemporaryDirectory temporary;
    const BuilderConfiguration configuration =
        synthetic_configuration(temporary.path(), "benchmark");
    auto report = benchmark_configuration(configuration);
    REQUIRE(report);
    CHECK(report.value().planned_tile_count == 6);
    CHECK(report.value().built_tile_count == 6);
    CHECK(report.value().reused_tile_count == 6);
    CHECK(report.value().pack_count == 6);
    CHECK(report.value().sampled_core_vertices ==
          6U * std::uint64_t{format_v1::core_vertices} * format_v1::core_vertices);
    CHECK(report.value().staging_io_bytes > 0);
    CHECK(report.value().stored_pack_bytes > 0);
    CHECK(report.value().compression_ratio > 0.0);
    CHECK(report.value().incremental_reuse_ratio == 1.0);
    CHECK(report.value().deterministic_rebuild);

    const auto output = temporary.path() / "benchmark.json";
    REQUIRE(write_benchmark_report(report.value(), output));
    const auto bytes = read_bytes(output);
    const std::string text{bytes.begin(), bytes.end()};
    CHECK(text.find("\"benchmark_schema\":\"lunar-terrain-m7-v1\"") != std::string::npos);
    CHECK(text.find("\"deterministic_rebuild\":true") != std::string::npos);
}

}  // namespace
}  // namespace lunar::terrain::builder
