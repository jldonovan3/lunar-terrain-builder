#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/configuration.hpp"
#include "builder/raster_source.hpp"

namespace lunar::terrain::builder {

struct ScanSourceReport {
    DatasetId dataset_id;
    std::string stable_key;
    std::string source_uri;
    std::int32_t priority{};
    RasterSourceRole role{RasterSourceRole::base};
    FusionPolicy fusion_policy{FusionPolicy::replace};
    double effective_resolution_meters{};
    std::string quality_mapping;
    std::vector<std::string> quality_members;
    std::vector<std::string> unsupported_quality_values;
    std::optional<RasterSourceDetails> raster_details;
    std::size_t raster_file_count{};
    std::vector<ArtifactMember> artifact_members;
    std::optional<std::uint64_t> artifact_bundle_bytes;
    std::optional<Sha256Digest> artifact_bundle_sha256;
    std::optional<double> center_elevation_meters;
};

struct ScanReport {
    std::string database_name;
    DatasetId dataset_id;
    std::string stable_key;
    std::string source_uri;
    Sha256Digest builder_configuration_hash;
    Sha256Digest semantic_configuration_hash;
    std::optional<RasterSourceDetails> raster_details;
    std::vector<ArtifactMember> artifact_members;
    std::optional<std::uint64_t> artifact_bundle_bytes;
    std::optional<Sha256Digest> artifact_bundle_sha256;
    std::optional<double> center_elevation_meters;
    std::vector<ScanSourceReport> sources;
    std::optional<GeographicBounds> required_region;
};

struct PlanSourceReport {
    DatasetId dataset_id;
    std::string stable_key;
    std::int32_t priority{};
    FusionPolicy fusion_policy{FusionPolicy::replace};
    GeographicBounds coverage;
    double effective_resolution_meters{};
    std::uint8_t target_level{};
};

struct PlanLevelCount {
    std::uint8_t level{};
    std::uint64_t tile_count{};
};

struct PlanReport {
    std::vector<LunarTileKey> tiles;
    std::vector<LunarTileKey> expected_hierarchy_tiles;
    std::uint64_t estimated_uncompressed_channel_bytes{};
    std::vector<PlanSourceReport> sources;
    std::vector<PlanLevelCount> level_counts;
    std::optional<GeographicBounds> required_region;
};

struct PackBuildReport {
    PackId id;
    std::filesystem::path path;
    Sha256Digest sha256;
    std::uint64_t bytes{};
};

struct BuildReport {
    std::filesystem::path database_path;
    Sha256Digest database_content_hash;
    Sha256Digest builder_configuration_hash;
    std::vector<PackBuildReport> packs;
    std::uint64_t tile_count{};
    std::uint64_t built_tile_count{};
    std::uint64_t reused_tile_count{};
};

struct BuildOptions {
    bool incremental{};
    std::stop_token cancellation;
};

struct ValidationReport {
    std::filesystem::path database_path;
    std::uint64_t tile_count{};
    std::uint32_t pack_count{};
    bool full{};
    std::uint64_t verified_projection_samples{};
    std::uint64_t verified_scientific_tiles{};
    std::uint64_t verified_provenance_tiles{};
    std::uint64_t verified_hierarchy_tiles{};
    std::uint64_t verified_seams{};
};

struct InspectionReport {
    std::filesystem::path database_path;
    std::uint64_t tile_count{};
    std::uint32_t dataset_count{};
    std::uint32_t pack_count{};
    Sha256Digest database_content_hash;
    std::optional<LunarTileKey> tile_key;
    std::optional<std::uint16_t> minimum_elevation_code;
    std::optional<std::uint16_t> maximum_elevation_code;
    std::optional<std::uint32_t> primary_dataset_id;
    std::optional<std::uint8_t> channel_count;
    std::optional<std::uint32_t> pack_id;
    std::optional<std::uint64_t> payload_offset;
    std::optional<std::uint32_t> stored_bytes;
    std::optional<std::uint32_t> effective_resolution_millimeters;
    std::optional<std::uint32_t> geometric_error_millimeters;
    std::optional<std::uint8_t> materialized_child_mask;
    std::optional<LunarTileKey> parent;
    std::vector<LunarTileKey> children;
    std::vector<DatasetId> contributing_datasets;
    std::optional<std::uint8_t> quality_flags;
    std::string content_hash_prefix;
    std::string dependency_hash_prefix;
};

struct DiffReport {
    std::filesystem::path before_path;
    std::filesystem::path after_path;
    bool dataset_registry_changed{};
    bool builder_configuration_changed{};
    bool package_layout_changed{};
    std::vector<DatasetId> added_datasets;
    std::vector<DatasetId> removed_datasets;
    std::vector<LunarTileKey> added_tiles;
    std::vector<LunarTileKey> removed_tiles;
    std::vector<LunarTileKey> dependency_changes;
    std::vector<LunarTileKey> content_changes;
    std::vector<LunarTileKey> provenance_changes;
    std::vector<LunarTileKey> package_layout_changes;

    [[nodiscard]] bool identical() const noexcept;
};

enum class DiagnosticExportFormat : std::uint8_t {
    ply,
    obj,
    elevation_pgm,
    sample_csv,
    raw_u16_le,
    provenance_ppm,
    quality_ppm,
    transition_csv,
};

struct BenchmarkReport {
    std::string host_platform;
    std::string compiler;
    std::string build_configuration;
    std::uint32_t worker_threads{};
    Sha256Digest builder_configuration_hash;
    std::uint64_t planned_tile_count{};
    std::uint64_t sampled_core_vertices{};
    std::uint64_t staging_io_bytes{};
    std::uint64_t peak_resident_memory_bytes{};
    std::uint64_t uncompressed_channel_bytes{};
    std::uint64_t stored_pack_bytes{};
    std::uint32_t pack_count{};
    std::uint64_t built_tile_count{};
    std::uint64_t reused_tile_count{};
    double catalog_seconds{};
    double clean_build_seconds{};
    double sampling_throughput_samples_per_second{};
    double staging_io_mebibytes_per_second{};
    double compression_ratio{};
    double validation_seconds{};
    double incremental_build_seconds{};
    double incremental_reuse_ratio{};
    bool deterministic_rebuild{};
};

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] Result<ScanReport> scan_configuration(const BuilderConfiguration& configuration);
[[nodiscard]] Result<PlanReport> plan_configuration(const BuilderConfiguration& configuration);
[[nodiscard]] Result<BuildReport> build_configuration(
    const BuilderConfiguration& configuration,
    BuildOptions options = {});
[[nodiscard]] Result<PlanReport> plan_synthetic(const BuilderConfiguration& configuration);
[[nodiscard]] Result<BuildReport> build_synthetic(
    const BuilderConfiguration& configuration,
    BuildOptions options = {});
[[nodiscard]] Result<BuildReport> build_raster_source(
    const BuilderConfiguration& configuration,
    BuildOptions options = {});
[[nodiscard]] Result<ValidationReport> validate_database(
    const std::filesystem::path& path,
    bool full);
[[nodiscard]] Result<InspectionReport> inspect_database(
    const std::filesystem::path& path,
    std::optional<LunarTileKey> key);
[[nodiscard]] Result<DiffReport> diff_databases(
    const std::filesystem::path& before_path,
    const std::filesystem::path& after_path);
[[nodiscard]] Result<void> export_tile(
    const std::filesystem::path& database_path,
    LunarTileKey key,
    DiagnosticExportFormat format,
    const std::filesystem::path& output_path);
[[nodiscard]] Result<void> export_tile_diagnostic(
    const std::filesystem::path& database_path,
    LunarTileKey key,
    DiagnosticExportFormat format,
    const std::filesystem::path& output_path);
[[nodiscard]] Result<BenchmarkReport> benchmark_configuration(
    const BuilderConfiguration& configuration);
[[nodiscard]] Result<void> write_benchmark_report(
    const BenchmarkReport& report,
    const std::filesystem::path& output_path);

[[nodiscard]] std::string format_report(const ScanReport& report, bool json);
[[nodiscard]] std::string format_report(const PlanReport& report, bool json);
[[nodiscard]] std::string format_report(const BuildReport& report, bool json);
[[nodiscard]] std::string format_report(const ValidationReport& report, bool json);
[[nodiscard]] std::string format_report(const InspectionReport& report, bool json);
[[nodiscard]] std::string format_report(const DiffReport& report, bool json);
[[nodiscard]] std::string format_report(const BenchmarkReport& report, bool json);

}  // namespace lunar::terrain::builder
