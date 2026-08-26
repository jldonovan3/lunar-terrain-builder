#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>

namespace lunar::terrain::builder {

enum class BuilderSourceKind : std::uint8_t {
    synthetic,
    raster,
};

enum class ElevationRepresentation : std::uint8_t {
    elevation_meters,
    radius_meters,
};

enum class NoDataPolicy : std::uint8_t {
    error,
    nearest_valid,
};

enum class RasterSourceRole : std::uint8_t {
    base,
    refinement,
};

enum class FusionPolicy : std::uint8_t {
    replace,
    bias_corrected_replace,
    residual_refinement_v1,
};

struct ArtifactMemberConfiguration {
    std::string name;
    std::optional<std::uint64_t> expected_bytes;
    std::optional<Sha256Digest> expected_sha256;
};

struct RasterConfiguration {
    std::string stable_key;
    std::string source_uri;
    std::string product_name;
    std::string producer;
    std::string mission;
    std::string instrument;
    std::string product_version;
    std::string original_crs;
    std::string license;
    std::string raster_member;
    std::string auxiliary_member;
    std::string label_member;
    std::string expected_data_type;
    std::vector<ArtifactMemberConfiguration> artifact_members;
    std::optional<std::uint64_t> expected_bundle_bytes;
    std::optional<Sha256Digest> expected_bundle_sha256;
    std::filesystem::path source_root;
    std::uint32_t expected_width{};
    std::uint32_t expected_height{};
    double west_longitude_degrees{};
    double east_longitude_degrees{};
    double south_latitude_degrees{};
    double north_latitude_degrees{};
    double nominal_resolution_meters{};
    double effective_resolution_meters{};
    double source_no_data{};
    double sample_scale{1.0};
    double sample_offset{};
    double source_reference_radius_meters{1'737'400.0};
    std::int32_t priority{};
    RasterSourceRole role{RasterSourceRole::base};
    FusionPolicy fusion_policy{FusionPolicy::replace};
    ElevationRepresentation elevation_representation{ElevationRepresentation::elevation_meters};
    NoDataPolicy no_data_policy{NoDataPolicy::error};
    bool metadata_override{};
};

struct BuilderConfiguration {
    std::filesystem::path source_path;
    std::filesystem::path output_directory;
    std::filesystem::path cache_directory;
    std::string database_name;
    BuilderSourceKind source_kind{BuilderSourceKind::synthetic};
    std::string synthetic_stable_key;
    std::string synthetic_source_uri;
    std::vector<RasterConfiguration> rasters;
    std::uint64_t target_pack_bytes{1'073'741'824ULL};
    std::uint32_t worker_threads{1};
    std::int32_t synthetic_amplitude_meters{2'048};
    std::uint8_t maximum_level{};
};

struct ConfigurationIdentity {
    std::string canonical_builder_json;
    std::string canonical_semantic_json;
    Sha256Digest builder_hash;
    Sha256Digest semantic_hash;
    std::vector<DatasetId> dataset_ids;
};

[[nodiscard]] Result<BuilderConfiguration> load_configuration(
    const std::filesystem::path& path);
[[nodiscard]] Result<ConfigurationIdentity> identify_configuration(
    const BuilderConfiguration& configuration);

}  // namespace lunar::terrain::builder
