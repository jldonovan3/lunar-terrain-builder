#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/configuration.hpp"

namespace lunar::terrain::builder {

struct ScanReport {
    std::string database_name;
    DatasetId dataset_id;
    std::string stable_key;
    std::string source_uri;
    Sha256Digest builder_configuration_hash;
    Sha256Digest semantic_configuration_hash;
};

struct PlanReport {
    std::vector<LunarTileKey> tiles;
    std::uint64_t estimated_uncompressed_channel_bytes{};
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
};

struct ValidationReport {
    std::filesystem::path database_path;
    std::uint64_t tile_count{};
    std::uint32_t pack_count{};
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
};

[[nodiscard]] std::string_view version_string() noexcept;
[[nodiscard]] Result<ScanReport> scan_configuration(const BuilderConfiguration& configuration);
[[nodiscard]] Result<PlanReport> plan_synthetic(const BuilderConfiguration& configuration);
[[nodiscard]] Result<BuildReport> build_synthetic(const BuilderConfiguration& configuration);
[[nodiscard]] Result<ValidationReport> validate_database(
    const std::filesystem::path& path,
    bool full);
[[nodiscard]] Result<InspectionReport> inspect_database(
    const std::filesystem::path& path,
    std::optional<LunarTileKey> key);

[[nodiscard]] std::string format_report(const ScanReport& report, bool json);
[[nodiscard]] std::string format_report(const PlanReport& report, bool json);
[[nodiscard]] std::string format_report(const BuildReport& report, bool json);
[[nodiscard]] std::string format_report(const ValidationReport& report, bool json);
[[nodiscard]] std::string format_report(const InspectionReport& report, bool json);

}  // namespace lunar::terrain::builder
