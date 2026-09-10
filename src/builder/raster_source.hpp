#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <lunar/terrain/coordinates.hpp>
#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/configuration.hpp"
#include "builder/telemetry.hpp"

namespace lunar::terrain::builder {

struct ArtifactMember {
    std::string name;
    std::uint64_t bytes{};
    Sha256Digest sha256;
};

struct DatasetArtifact {
    DatasetId id;
    std::uint32_t flags{};
    std::array<std::string, 8> strings;
    double nominal_resolution_meters{};
    double effective_resolution_meters{};
    double horizontal_accuracy_meters{};
    double vertical_accuracy_meters{};
    double source_no_data{};
    std::vector<ArtifactMember> artifact_members;
    std::uint64_t artifact_bundle_bytes{};
    Sha256Digest artifact_bundle_hash;
    std::string metadata_json;
    Sha256Digest registry_hash;
    std::string datum_version;
    std::string sampling_algorithm;
};

using GeographicFootprint = GeographicBounds;

struct RawTerrainSample {
    double elevation_meters{};
    bool interpolated{};
    bool filled_no_data{};
    std::uint8_t quality_flags{};
};

struct RasterSourceDetails {
    std::string driver_name;
    std::string data_type_name;
    std::uint32_t width{};
    std::uint32_t height{};
    double no_data_value{};
    double sample_scale{};
    double sample_offset{};
    ElevationRepresentation elevation_representation{ElevationRepresentation::elevation_meters};
    GeographicFootprint footprint;
};

class IRasterSource {
public:
    virtual ~IRasterSource() = default;

    [[nodiscard]] virtual const DatasetArtifact& metadata() const noexcept = 0;
    [[nodiscard]] virtual const RasterSourceDetails& details() const noexcept = 0;
    [[nodiscard]] virtual bool Covers(LunarGeodeticCoordinate coordinate) const noexcept = 0;
    [[nodiscard]] virtual Result<RawTerrainSample> Sample(
        LunarGeodeticCoordinate coordinate) const = 0;
    // Fusion treats coverage gaps and strict source no-data as absence so a
    // lower-priority source can remain authoritative there. Other failures are
    // still reported.
    [[nodiscard]] virtual Result<std::optional<RawTerrainSample>> TrySample(
        LunarGeodeticCoordinate coordinate) const;
    [[nodiscard]] virtual Result<std::vector<std::optional<RawTerrainSample>>> SampleBatch(
        std::span<const LunarGeodeticCoordinate> coordinates,
        std::stop_token cancellation = {}) const;
    [[nodiscard]] virtual Result<Sha256Digest> WindowDependency(
        LunarTileKey key) const = 0;
};

class CoverageIndex {
public:
    explicit CoverageIndex(std::span<const IRasterSource* const> sources);

    [[nodiscard]] std::vector<const IRasterSource*> At(
        LunarGeodeticCoordinate coordinate) const;
    [[nodiscard]] Result<std::vector<const IRasterSource*>> Covering(
        LunarTileKey key) const;

private:
    std::vector<const IRasterSource*> sources_;
};

struct PreparedSourceCatalog {
    ConfigurationIdentity identity;
    std::vector<std::unique_ptr<IRasterSource>> sources;
};

[[nodiscard]] Result<PreparedSourceCatalog> prepare_source_catalog(
    const BuilderConfiguration& configuration,
    TelemetryCollector* telemetry = nullptr);

[[nodiscard]] Result<std::unique_ptr<IRasterSource>> open_raster_source(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    TelemetryCollector* telemetry = nullptr);

[[nodiscard]] Result<std::vector<std::unique_ptr<IRasterSource>>> open_raster_sources(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    TelemetryCollector* telemetry = nullptr);

[[nodiscard]] Result<LunarTileKey> choose_raster_prototype_tile(
    const IRasterSource& source,
    std::uint8_t level);

}  // namespace lunar::terrain::builder
