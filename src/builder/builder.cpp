#include "builder/builder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <numbers>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/qsc_topology.hpp>

#include "builder/hierarchy.hpp"

namespace lunar::terrain::builder {
namespace {

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() + 2U);
    encoded.push_back('"');
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (const unsigned char character : value) {
        switch (character) {
            case '"': encoded += "\\\""; break;
            case '\\': encoded += "\\\\"; break;
            case '\b': encoded += "\\b"; break;
            case '\t': encoded += "\\t"; break;
            case '\n': encoded += "\\n"; break;
            case '\f': encoded += "\\f"; break;
            case '\r': encoded += "\\r"; break;
            default:
                if (character < 0x20U) {
                    encoded += "\\u00";
                    encoded.push_back(hex[character >> 4U]);
                    encoded.push_back(hex[character & 0x0FU]);
                } else {
                    encoded.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] std::string_view fusion_policy_name(const FusionPolicy policy) noexcept {
    switch (policy) {
        case FusionPolicy::replace: return "Replace";
        case FusionPolicy::bias_corrected_replace: return "BiasCorrectedReplace";
        case FusionPolicy::residual_refinement_v1: return "ResidualRefinement_v1";
    }
    return "Replace";
}

[[nodiscard]] std::string optional_double_json(const std::optional<double> value) {
    return value ? fmt::format("{}", *value) : "null";
}

[[nodiscard]] std::string double_json(const double value) {
    return std::isnan(value) ? json_string("nan") : fmt::format("{}", value);
}

[[nodiscard]] std::string bounds_json(const GeographicBounds& bounds) {
    return fmt::format(
        "{{\"east\":{},\"north\":{},\"south\":{},\"west\":{}}}",
        bounds.east_longitude_degrees,
        bounds.north_latitude_degrees,
        bounds.south_latitude_degrees,
        bounds.west_longitude_degrees);
}

[[nodiscard]] std::string string_array_json(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += json_string(values[index]);
    }
    return result;
}

[[nodiscard]] Error validation_error(
    std::string message,
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key = std::nullopt) {
    Error error{ErrorCode::invalid_format, std::move(message)};
    error.with_path(path.string());
    if (key) {
        error.with_tile_key(key->encoded());
    }
    return error;
}

[[nodiscard]] const DecodedChannel* elevation_channel(const DecodedTerrainTile& tile) noexcept {
    const auto found = std::ranges::find_if(tile.channels(), [](const DecodedChannel& channel) {
        return channel.id() == ChannelId::elevation;
    });
    return found == tile.channels().end() ? nullptr : &*found;
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t sample_index) noexcept {
    const std::size_t offset = sample_index * 2U;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

template <std::size_t Size>
[[nodiscard]] std::string bytes_to_hex(const std::array<std::byte, Size>& bytes) {
    constexpr std::array digits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string result;
    result.reserve(Size * 2U);
    for (const std::byte byte : bytes) {
        const std::uint8_t value = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[value >> 4U]);
        result.push_back(digits[value & 0x0FU]);
    }
    return result;
}

[[nodiscard]] std::size_t serialized_edge_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    constexpr std::size_t width = format_v1::serialized_elevation_samples;
    const std::size_t core_parameter = std::size_t{parameter} + 1U;
    switch (edge) {
        case QscEdge::west:
            return core_parameter * width + 1U;
        case QscEdge::east:
            return core_parameter * width + format_v1::core_vertices;
        case QscEdge::south:
            return width + core_parameter;
        case QscEdge::north:
            return std::size_t{format_v1::core_vertices} * width + core_parameter;
    }
    return 0;
}

[[nodiscard]] Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>> read_all_tiles(
    LunarTerrainDatabase& database,
    const std::filesystem::path& path,
    const ExecutionOptions& options) {
    std::vector<TileIndexEntry> entries;
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
                std::move(checked).error());
        }
        auto root = LunarTileKey::create(face, 0, 0, 0);
        if (!root) {
            return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
                std::move(root).error());
        }
        auto subtree = database.QuerySubtree(root.value());
        entries.insert(entries.end(), subtree.begin(), subtree.end());
    }
    std::ranges::sort(entries, {}, [](const TileIndexEntry& entry) { return entry.key; });
    if (entries.size() != database.Header().tile_count) {
        return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
            validation_error("tile index contains an unreachable or duplicated QSC tile", path));
    }

    std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>> tiles;
    tiles.reserve(entries.size());
    for (std::size_t index = 0; index < entries.size(); ++index) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
                std::move(checked).error());
        }
        const TileIndexEntry& entry = entries[index];
        auto decoded = database.ReadTile(entry.key);
        if (!decoded) {
            return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
                std::move(decoded).error());
        }
        tiles.emplace_back(entry, std::move(decoded).value());
        if (options.telemetry != nullptr) {
            options.telemetry->SetWork(index + 1U, entries.size());
            options.telemetry->AddCount("validation_decoded_tiles");
        }
    }
    return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::success(
        std::move(tiles));
}

[[nodiscard]] Result<std::uint64_t> validate_seams(
    const std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>& tiles,
    const std::filesystem::path& path,
    const ExecutionOptions& options) {
    std::map<std::uint64_t, const DecodedTerrainTile*> by_key;
    for (const auto& [entry, tile] : tiles) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<std::uint64_t>::failure(std::move(checked).error());
        }
        by_key.emplace(entry.key.encoded(), &tile);
    }

    constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    std::uint64_t verified = 0;
    for (const auto& [entry, tile] : tiles) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<std::uint64_t>::failure(std::move(checked).error());
        }
        const DecodedChannel* source = elevation_channel(tile);
        if (source == nullptr) {
            return Result<std::uint64_t>::failure(
                validation_error("tile omits the required elevation channel", path, entry.key));
        }
        for (const QscEdge edge : edges) {
            auto neighbor = qsc_tile_neighbor(entry.key, edge);
            if (!neighbor) {
                return Result<std::uint64_t>::failure(std::move(neighbor).error());
            }
            const auto found = by_key.find(neighbor.value().key.encoded());
            if (found == by_key.end() || entry.key >= neighbor.value().key) {
                continue;
            }
            const DecodedChannel* destination = elevation_channel(*found->second);
            if (destination == nullptr) {
                return Result<std::uint64_t>::failure(validation_error(
                    "neighbor tile omits the required elevation channel", path, neighbor.value().key));
            }
            for (std::uint16_t parameter = 0; parameter < format_v1::core_vertices; ++parameter) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                const std::uint16_t source_value = read_u16(
                    source->bytes(), serialized_edge_index(edge, parameter));
                const std::uint16_t destination_value = read_u16(
                    destination->bytes(),
                    serialized_edge_index(neighbor.value().touching_edge, mapped));
                if (source_value != destination_value) {
                    return Result<std::uint64_t>::failure(validation_error(
                        "same-level QSC edge samples are not byte-identical", path, entry.key));
                }
            }
            ++verified;
        }
    }
    return Result<std::uint64_t>::success(verified);
}

[[nodiscard]] Result<void> validate_hierarchy(
    const std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>& tiles,
    const std::filesystem::path& path,
    const ExecutionOptions& options) {
    std::map<std::uint64_t, const TileIndexEntry*> by_key;
    for (const auto& [entry, unused_tile] : tiles) {
        auto checked = check_execution(options);
        if (!checked) {
            return checked;
        }
        static_cast<void>(unused_tile);
        by_key.emplace(entry.key.encoded(), &entry);
    }
    for (const auto& [entry, unused_tile] : tiles) {
        auto checked = check_execution(options);
        if (!checked) {
            return checked;
        }
        static_cast<void>(unused_tile);
        std::uint8_t expected_mask = 0;
        auto children = entry.key.children();
        if (children) {
            for (std::uint8_t quadrant = 0; quadrant < children.value().size(); ++quadrant) {
                if (by_key.contains(children.value()[quadrant].encoded())) {
                    expected_mask = static_cast<std::uint8_t>(
                        expected_mask | static_cast<std::uint8_t>(1U << quadrant));
                }
            }
        }
        if (expected_mask != entry.materialized_child_mask) {
            return Result<void>::failure(validation_error(
                "materialized child mask disagrees with the tile index", path, entry.key));
        }
        // M3/M5 emitted a single regional prototype tile. Preserve validation
        // of that historical fixture while requiring connectivity for every
        // multi-node sparse hierarchy built from M6 onward.
        if (tiles.size() > 1 && entry.key.level() != 0) {
            const auto parent = entry.key.parent();
            if (!parent || !by_key.contains(parent->encoded())) {
                return Result<void>::failure(validation_error(
                    "sparse hierarchy contains an orphan tile", path, entry.key));
            }
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::uint64_t> validate_projection(
    const std::filesystem::path& path,
    const ExecutionOptions& options) {
    constexpr std::array samples{
        std::array{-0.875, -0.625},
        std::array{-0.5, 0.25},
        std::array{0.0, 0.0},
        std::array{0.375, -0.75},
        std::array{0.8125, 0.6875},
    };
    std::uint64_t verified = 0;
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<std::uint64_t>::failure(std::move(checked).error());
        }
        for (const auto& sample : samples) {
            const QscCoordinate qsc{
                static_cast<QscFace>(face), sample[0], sample[1], 123.5};
            auto geographic = QscProjection::Inverse(qsc);
            if (!geographic) {
                return Result<std::uint64_t>::failure(std::move(geographic).error());
            }
            auto round_trip = QscProjection::Forward(geographic.value());
            if (!round_trip) {
                return Result<std::uint64_t>::failure(std::move(round_trip).error());
            }
            if (round_trip.value().face != qsc.face ||
                std::abs(round_trip.value().u - qsc.u) > 5.0e-13 ||
                std::abs(round_trip.value().v - qsc.v) > 5.0e-13 ||
                round_trip.value().elevation_meters != qsc.elevation_meters) {
                return Result<std::uint64_t>::failure(validation_error(
                    "deterministic QSC projection round trip exceeded tolerance", path));
            }
            ++verified;
        }
    }
    return Result<std::uint64_t>::success(verified);
}

struct ScientificValidationCounts {
    std::uint64_t scientific_tiles{};
    std::uint64_t provenance_tiles{};
};

[[nodiscard]] Result<ScientificValidationCounts> validate_scientific_content(
    const std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>& tiles,
    const std::span<const DatasetId> datasets,
    const std::filesystem::path& path,
    const ExecutionOptions& options) {
    std::set<std::uint32_t> dataset_ids;
    for (const DatasetId dataset : datasets) {
        dataset_ids.insert(dataset.value);
    }

    ScientificValidationCounts counts;
    for (const auto& [entry, tile] : tiles) {
        auto checked = check_execution(options);
        if (!checked) {
            return Result<ScientificValidationCounts>::failure(std::move(checked).error());
        }
        const DecodedChannel* elevation = elevation_channel(tile);
        const auto& metadata = tile.metadata();
        const std::size_t elevation_bytes =
            std::size_t{format_v1::serialized_elevation_samples} *
            format_v1::serialized_elevation_samples * 2U;
        if (elevation == nullptr ||
            elevation->width() != format_v1::serialized_elevation_samples ||
            elevation->height() != format_v1::serialized_elevation_samples ||
            elevation->element_type() != ElementType::u16 ||
            elevation->bytes().size() != elevation_bytes ||
            !std::isfinite(metadata.effective_resolution_meters) ||
            metadata.effective_resolution_meters <= 0.0F ||
            !std::isfinite(metadata.geometric_error_meters) ||
            metadata.geometric_error_meters < 0.0F ||
            !std::isfinite(metadata.minimum_elevation_meters) ||
            !std::isfinite(metadata.maximum_elevation_meters) ||
            metadata.minimum_elevation_meters > metadata.maximum_elevation_meters ||
            !dataset_ids.contains(metadata.primary_dataset.value)) {
            return Result<ScientificValidationCounts>::failure(validation_error(
                "tile scientific values or canonical elevation dimensions are invalid", path, entry.key));
        }
        ++counts.scientific_tiles;

        if (!tile.provenance() || tile.provenance()->palette.empty()) {
            return Result<ScientificValidationCounts>::failure(validation_error(
                "tile omits required provenance", path, entry.key));
        }
        const TileProvenance& provenance = *tile.provenance();
        const bool has_map = !provenance.dominant_source_indices.empty();
        if ((has_map &&
             (provenance.map_width != format_v1::auxiliary_map_samples ||
              provenance.map_height != format_v1::auxiliary_map_samples ||
              provenance.dominant_source_indices.size() !=
                  std::size_t{format_v1::auxiliary_map_samples} *
                      format_v1::auxiliary_map_samples)) ||
            (!has_map && (provenance.map_width != 0 || provenance.map_height != 0))) {
            return Result<ScientificValidationCounts>::failure(validation_error(
                "provenance map dimensions are not canonical", path, entry.key));
        }
        for (const ProvenancePaletteEntry& palette_entry : provenance.palette) {
            if (!dataset_ids.contains(palette_entry.dataset_id.value)) {
                return Result<ScientificValidationCounts>::failure(validation_error(
                    "provenance palette references an unknown DatasetID", path, entry.key));
            }
        }
        const auto quality = std::ranges::find_if(
            tile.channels(), [](const DecodedChannel& channel) {
                return channel.id() == ChannelId::quality;
            });
        if (quality != tile.channels().end() &&
            (quality->width() != format_v1::auxiliary_map_samples ||
             quality->height() != format_v1::auxiliary_map_samples ||
             quality->element_type() != ElementType::u8 ||
             quality->bytes().size() !=
                 std::size_t{format_v1::auxiliary_map_samples} *
                     format_v1::auxiliary_map_samples)) {
            return Result<ScientificValidationCounts>::failure(validation_error(
                "quality map dimensions are not canonical", path, entry.key));
        }
        ++counts.provenance_tiles;
    }
    return Result<ScientificValidationCounts>::success(counts);
}

}  // namespace

std::string_view version_string() noexcept {
    return "0.3.0";
}

Result<ScanReport> scan_configuration(
    const BuilderConfiguration& configuration,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase(
            "scan",
            0,
            configuration.source_kind == BuilderSourceKind::raster
                ? configuration.rasters.size()
                : 1U);
    }
    TelemetryActivity scan_activity{options.telemetry, "scan"};
    auto execution = check_execution(options);
    if (!execution) {
        return Result<ScanReport>::failure(std::move(execution).error());
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<ScanReport>::failure(std::move(identity).error());
    }
    std::size_t representative = 0;
    if (configuration.source_kind == BuilderSourceKind::raster) {
        for (std::size_t index = 1; index < configuration.rasters.size(); ++index) {
            if (std::tuple{configuration.rasters[index].priority,
                           identity.value().dataset_ids[index].value} <
                std::tuple{configuration.rasters[representative].priority,
                           identity.value().dataset_ids[representative].value}) {
                representative = index;
            }
        }
    }
    ScanReport report{
        configuration.database_name,
        identity.value().dataset_ids[representative],
        configuration.source_kind == BuilderSourceKind::synthetic
            ? configuration.synthetic_stable_key : configuration.rasters[representative].stable_key,
        configuration.source_kind == BuilderSourceKind::synthetic
            ? configuration.synthetic_source_uri : configuration.rasters[representative].source_uri,
        identity.value().builder_hash,
        identity.value().semantic_hash,
    };
    if (configuration.source_kind == BuilderSourceKind::raster) {
        std::vector<std::unique_ptr<IRasterSource>> owned_sources;
        const std::vector<std::unique_ptr<IRasterSource>>* sources = nullptr;
        if (options.prepared_source_catalog != nullptr) {
            if (options.prepared_source_catalog->identity.builder_hash != identity.value().builder_hash ||
                options.prepared_source_catalog->sources.size() != configuration.rasters.size()) {
                return Result<ScanReport>::failure(Error{
                    ErrorCode::invalid_argument,
                    "prepared source catalog does not match the configuration"});
            }
            sources = &options.prepared_source_catalog->sources;
        } else {
            auto opened = open_raster_sources(configuration, identity.value(), options.telemetry);
            if (!opened) {
                return Result<ScanReport>::failure(std::move(opened).error());
            }
            owned_sources = std::move(opened).value();
            sources = &owned_sources;
        }
        const IRasterSource& source = *(*sources)[representative];
        report.raster_details = source.details();
        report.artifact_members = source.metadata().artifact_members;
        report.artifact_bundle_bytes = source.metadata().artifact_bundle_bytes;
        report.artifact_bundle_sha256 = source.metadata().artifact_bundle_hash;
        const GeographicFootprint& footprint = source.details().footprint;
        double longitude = (footprint.west_longitude_degrees +
                            footprint.east_longitude_degrees) * 0.5;
        if (longitude > 180.0) {
            longitude -= 360.0;
        }
        const double latitude = (footprint.south_latitude_degrees +
                                 footprint.north_latitude_degrees) * 0.5;
        auto sample = [&]() {
            TelemetryActivity activity{options.telemetry, "sampling"};
            if (options.telemetry != nullptr) {
                options.telemetry->AddCount("requested_source_samples");
            }
            return source.TrySample(LunarGeodeticCoordinate{
                latitude * std::numbers::pi_v<double> / 180.0,
                longitude * std::numbers::pi_v<double> / 180.0,
                0.0});
        }();
        if (!sample) {
            return Result<ScanReport>::failure(std::move(sample).error());
        }
        if (sample.value()) {
            report.center_elevation_meters = sample.value()->elevation_meters;
        }

        std::vector<std::size_t> order(configuration.rasters.size());
        for (std::size_t index = 0; index < order.size(); ++index) {
            order[index] = index;
        }
        std::ranges::sort(order, {}, [&](const std::size_t index) {
            return std::tuple{configuration.rasters[index].priority,
                              identity.value().dataset_ids[index].value};
        });
        report.sources.reserve(order.size());
        for (const std::size_t index : order) {
            execution = check_execution(options);
            if (!execution) {
                return Result<ScanReport>::failure(std::move(execution).error());
            }
            const RasterConfiguration& raster = configuration.rasters[index];
            const IRasterSource& raster_source = *(*sources)[index];
            ScanSourceReport source_report;
            source_report.dataset_id = identity.value().dataset_ids[index];
            source_report.stable_key = raster.stable_key;
            source_report.source_uri = raster.source_uri;
            source_report.priority = raster.priority;
            source_report.role = raster.role;
            source_report.fusion_policy = raster.fusion_policy;
            source_report.effective_resolution_meters = raster.effective_resolution_meters;
            source_report.quality_mapping = raster.quality_mapping;
            source_report.quality_members = raster.quality_members;
            source_report.unsupported_quality_values = raster.unsupported_quality_values;
            source_report.raster_details = raster_source.details();
            source_report.raster_file_count = raster.raster_files.size();
            source_report.artifact_members = raster_source.metadata().artifact_members;
            source_report.artifact_bundle_bytes = raster_source.metadata().artifact_bundle_bytes;
            source_report.artifact_bundle_sha256 = raster_source.metadata().artifact_bundle_hash;
            const GeographicFootprint& source_footprint = raster_source.details().footprint;
            double source_longitude = (source_footprint.west_longitude_degrees +
                                       source_footprint.east_longitude_degrees) * 0.5;
            if (source_longitude > 180.0) {
                source_longitude -= 360.0;
            }
            const double source_latitude = (source_footprint.south_latitude_degrees +
                                            source_footprint.north_latitude_degrees) * 0.5;
            auto center = [&]() {
                TelemetryActivity activity{options.telemetry, "sampling"};
                if (options.telemetry != nullptr) {
                    options.telemetry->AddCount("requested_source_samples");
                }
                return raster_source.TrySample(LunarGeodeticCoordinate{
                    source_latitude * std::numbers::pi_v<double> / 180.0,
                    source_longitude * std::numbers::pi_v<double> / 180.0,
                    0.0});
            }();
            if (!center) {
                return Result<ScanReport>::failure(std::move(center).error());
            }
            if (center.value()) {
                source_report.center_elevation_meters = center.value()->elevation_meters;
            }
            report.sources.push_back(std::move(source_report));
            if (options.telemetry != nullptr) {
                options.telemetry->SetWork(report.sources.size(), order.size());
            }
        }
        report.required_region = configuration.required_region;
    } else if (options.telemetry != nullptr) {
        options.telemetry->SetWork(1, 1);
    }
    return Result<ScanReport>::success(std::move(report));
}

Result<PlanReport> plan_configuration(
    const BuilderConfiguration& configuration,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase("plan");
    }
    TelemetryActivity plan_activity{options.telemetry, "plan"};
    auto execution = check_execution(options);
    if (!execution) {
        return Result<PlanReport>::failure(std::move(execution).error());
    }
    if (configuration.source_kind == BuilderSourceKind::synthetic) {
        auto report = plan_synthetic(configuration);
        if (report && options.telemetry != nullptr) {
            options.telemetry->SetWork(
                report.value().expected_hierarchy_tiles.size(),
                report.value().expected_hierarchy_tiles.size());
            options.telemetry->AddCount(
                "planned_tiles", report.value().expected_hierarchy_tiles.size());
        }
        return report;
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<PlanReport>::failure(std::move(identity).error());
    }
    std::vector<std::unique_ptr<IRasterSource>> owned_sources;
    const std::vector<std::unique_ptr<IRasterSource>>* sources = nullptr;
    if (options.prepared_source_catalog != nullptr) {
        if (options.prepared_source_catalog->identity.builder_hash != identity.value().builder_hash ||
            options.prepared_source_catalog->sources.size() != configuration.rasters.size()) {
            return Result<PlanReport>::failure(Error{
                ErrorCode::invalid_argument,
                "prepared source catalog does not match the configuration"});
        }
        sources = &options.prepared_source_catalog->sources;
    } else {
        auto opened = open_raster_sources(configuration, identity.value(), options.telemetry);
        if (!opened) {
            return Result<PlanReport>::failure(std::move(opened).error());
        }
        owned_sources = std::move(opened).value();
        sources = &owned_sources;
    }
    std::vector<HierarchySource> hierarchy_sources;
    hierarchy_sources.reserve(sources->size());
    for (std::size_t index = 0; index < sources->size(); ++index) {
        execution = check_execution(options);
        if (!execution) {
            return Result<PlanReport>::failure(std::move(execution).error());
        }
        GeographicFootprint footprint = (*sources)[index]->details().footprint;
        if (configuration.required_region) {
            footprint.west_longitude_degrees = std::max(
                footprint.west_longitude_degrees,
                configuration.required_region->west_longitude_degrees);
            footprint.east_longitude_degrees = std::min(
                footprint.east_longitude_degrees,
                configuration.required_region->east_longitude_degrees);
            footprint.south_latitude_degrees = std::max(
                footprint.south_latitude_degrees,
                configuration.required_region->south_latitude_degrees);
            footprint.north_latitude_degrees = std::min(
                footprint.north_latitude_degrees,
                configuration.required_region->north_latitude_degrees);
        }
        if (footprint.west_longitude_degrees >= footprint.east_longitude_degrees ||
            footprint.south_latitude_degrees >= footprint.north_latitude_degrees) {
            continue;
        }
        hierarchy_sources.push_back(HierarchySource{
            identity.value().dataset_ids[index],
            footprint,
            (*sources)[index]->metadata().effective_resolution_meters,
            configuration.maximum_level});
    }
    if (hierarchy_sources.empty()) {
        return Result<PlanReport>::failure(Error{
            ErrorCode::invalid_argument, "required region does not intersect any configured source"});
    }
    auto hierarchy = [&]() {
        TelemetryActivity activity{options.telemetry, "projection"};
        return plan_sparse_hierarchy(hierarchy_sources);
    }();
    if (!hierarchy) {
        return Result<PlanReport>::failure(std::move(hierarchy).error());
    }
    constexpr std::uint64_t elevation_bytes =
        std::uint64_t{format_v1::serialized_elevation_samples} *
        format_v1::serialized_elevation_samples * 2U;
    const std::uint64_t provenance_bytes =
        format_v1::bytes::provenance_header +
        configuration.rasters.size() * format_v1::bytes::provenance_palette_entry +
        (configuration.rasters.size() > 1 ? 64U * 64U : 0U);
    const std::uint64_t quality_bytes = configuration.rasters.size() > 1 ? 64U * 64U : 0U;
    const std::uint64_t bytes_per_tile = elevation_bytes + provenance_bytes + quality_bytes;
    if (hierarchy.value().tiles.size() >
        std::numeric_limits<std::uint64_t>::max() / bytes_per_tile) {
        return Result<PlanReport>::failure(Error{
            ErrorCode::arithmetic_overflow, "planned hierarchy byte estimate overflows uint64"});
    }
    SparseHierarchyPlan hierarchy_plan = std::move(hierarchy).value();
    PlanReport report;
    report.expected_hierarchy_tiles = std::move(hierarchy_plan.tiles);
    std::size_t target = 0;
    for (std::size_t index = 1; index < configuration.rasters.size(); ++index) {
        if (std::tuple{configuration.rasters[target].priority,
                       identity.value().dataset_ids[target].value} <
            std::tuple{configuration.rasters[index].priority,
                       identity.value().dataset_ids[index].value}) {
            target = index;
        }
    }
    const auto target_level = std::ranges::find(
        hierarchy_plan.source_levels,
        identity.value().dataset_ids[target],
        &HierarchySourceLevel::dataset_id);
    if (target_level != hierarchy_plan.source_levels.end()) {
        auto prototype = choose_raster_prototype_tile(
            *(*sources)[target], target_level->level);
        if (!prototype) {
            return Result<PlanReport>::failure(std::move(prototype).error());
        }
        report.tiles.push_back(prototype.value());
    }
    report.estimated_uncompressed_channel_bytes =
        static_cast<std::uint64_t>(report.expected_hierarchy_tiles.size()) * bytes_per_tile;
    report.required_region = configuration.required_region;
    for (const HierarchySourceLevel& level : hierarchy_plan.source_levels) {
        const auto found = std::ranges::find(identity.value().dataset_ids, level.dataset_id);
        if (found == identity.value().dataset_ids.end()) {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(found - identity.value().dataset_ids.begin());
        const HierarchySource& planned = *std::ranges::find(
            hierarchy_sources, level.dataset_id, &HierarchySource::dataset_id);
        report.sources.push_back(PlanSourceReport{
            level.dataset_id,
            configuration.rasters[index].stable_key,
            configuration.rasters[index].priority,
            configuration.rasters[index].fusion_policy,
            planned.footprint,
            planned.effective_resolution_meters,
            level.level});
    }
    std::ranges::sort(report.sources, {}, [](const PlanSourceReport& source) {
        return std::tuple{source.priority, source.dataset_id.value};
    });
    std::map<std::uint8_t, std::uint64_t> counts;
    for (const LunarTileKey key : report.expected_hierarchy_tiles) {
        execution = check_execution(options);
        if (!execution) {
            return Result<PlanReport>::failure(std::move(execution).error());
        }
        ++counts[key.level()];
    }
    for (const auto [level, count] : counts) {
        report.level_counts.push_back(PlanLevelCount{level, count});
    }
    if (options.telemetry != nullptr) {
        options.telemetry->SetWork(
            report.expected_hierarchy_tiles.size(), report.expected_hierarchy_tiles.size());
        options.telemetry->AddCount("planned_tiles", report.expected_hierarchy_tiles.size());
    }
    return Result<PlanReport>::success(std::move(report));
}

Result<BuildReport> build_configuration(
    const BuilderConfiguration& configuration,
    BuildOptions options) {
    if (!options.execution.cancellation.stop_possible()) {
        options.execution.cancellation = options.cancellation;
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("build");
    }
    auto execution = check_execution(options.execution);
    if (!execution) {
        return Result<BuildReport>::failure(std::move(execution).error());
    }
    return configuration.source_kind == BuilderSourceKind::synthetic
        ? build_synthetic(configuration, options)
        : build_raster_source(configuration, options);
}

Result<PlanReport> plan_synthetic(const BuilderConfiguration& configuration) {
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<PlanReport>::failure(std::move(identity).error());
    }
    PlanReport report;
    report.tiles.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto key = LunarTileKey::create(face, 0, 0, 0);
        if (!key) {
            return Result<PlanReport>::failure(std::move(key).error());
        }
        report.tiles.push_back(key.value());
    }
    report.expected_hierarchy_tiles = report.tiles;
    report.level_counts.push_back(PlanLevelCount{0, report.tiles.size()});
    constexpr std::uint64_t elevation_bytes =
        std::uint64_t{format_v1::serialized_elevation_samples} *
        format_v1::serialized_elevation_samples * 2U;
    constexpr std::uint64_t provenance_bytes =
        format_v1::bytes::provenance_header + format_v1::bytes::provenance_palette_entry;
    report.estimated_uncompressed_channel_bytes =
        report.tiles.size() * (elevation_bytes + provenance_bytes);
    return Result<PlanReport>::success(std::move(report));
}

Result<ValidationReport> validate_database(
    const std::filesystem::path& path,
    const bool full,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase("validation_open");
    }
    TelemetryActivity validation_activity{options.telemetry, "validation"};
    auto execution = check_execution(options);
    if (!execution) {
        return Result<ValidationReport>::failure(std::move(execution).error());
    }
    auto database = LunarTerrainDatabase::Open(path);
    if (!database) {
        return Result<ValidationReport>::failure(std::move(database).error());
    }
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase(
            "validation_decode", 0, database.value().Header().tile_count);
    }
    auto tiles = read_all_tiles(database.value(), path, options);
    if (!tiles) {
        return Result<ValidationReport>::failure(std::move(tiles).error());
    }
    std::uint64_t projection_samples = 0;
    std::uint64_t scientific_tiles = 0;
    std::uint64_t provenance_tiles = 0;
    std::uint64_t hierarchy_tiles = 0;
    std::uint64_t seams = 0;
    if (full) {
        if (options.telemetry != nullptr) {
            options.telemetry->SetPhase("validation_projection");
        }
        auto projection = [&]() {
            TelemetryActivity projection_activity{options.telemetry, "projection"};
            return validate_projection(path, options);
        }();
        if (!projection) {
            return Result<ValidationReport>::failure(std::move(projection).error());
        }
        projection_samples = projection.value();
        const std::vector<DatasetId> datasets = database.value().DatasetIds();
        if (options.telemetry != nullptr) {
            options.telemetry->SetPhase(
                "validation_scientific", 0, tiles.value().size());
        }
        auto scientific = validate_scientific_content(tiles.value(), datasets, path, options);
        if (!scientific) {
            return Result<ValidationReport>::failure(std::move(scientific).error());
        }
        scientific_tiles = scientific.value().scientific_tiles;
        provenance_tiles = scientific.value().provenance_tiles;
        if (options.telemetry != nullptr) {
            options.telemetry->SetPhase(
                "validation_hierarchy", 0, tiles.value().size());
        }
        auto hierarchy = validate_hierarchy(tiles.value(), path, options);
        if (!hierarchy) {
            return Result<ValidationReport>::failure(std::move(hierarchy).error());
        }
        hierarchy_tiles = tiles.value().size();
        if (options.telemetry != nullptr) {
            options.telemetry->SetPhase("validation_seams", 0, tiles.value().size());
        }
        TelemetryActivity seam_activity{options.telemetry, "seam_resolution"};
        auto validated = validate_seams(tiles.value(), path, options);
        if (!validated) {
            return Result<ValidationReport>::failure(std::move(validated).error());
        }
        seams = validated.value();
    }
    return Result<ValidationReport>::success(ValidationReport{
        path,
        database.value().Header().tile_count,
        database.value().Header().pack_count,
        full,
        projection_samples,
        scientific_tiles,
        provenance_tiles,
        hierarchy_tiles,
        seams,
    });
}

Result<InspectionReport> inspect_database(
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase("inspection");
    }
    TelemetryActivity inspection_activity{options.telemetry, "inspection"};
    auto execution = check_execution(options);
    if (!execution) {
        return Result<InspectionReport>::failure(std::move(execution).error());
    }
    auto database = LunarTerrainDatabase::Open(path);
    if (!database) {
        return Result<InspectionReport>::failure(std::move(database).error());
    }
    InspectionReport report{
        path,
        database.value().Header().tile_count,
        database.value().Header().dataset_count,
        database.value().Header().pack_count,
        database.value().Header().database_content_hash,
    };
    if (key) {
        const auto entry = database.value().FindTile(*key);
        if (!entry) {
            return Result<InspectionReport>::failure(
                Error{ErrorCode::not_found, "tile is not present in the database"}
                    .with_path(path.string())
                    .with_tile_key(key->encoded()));
        }
        auto tile = database.value().ReadTile(*key);
        if (!tile) {
            return Result<InspectionReport>::failure(std::move(tile).error());
        }
        report.tile_key = *key;
        report.minimum_elevation_code = entry->minimum_elevation_code;
        report.maximum_elevation_code = entry->maximum_elevation_code;
        report.primary_dataset_id = entry->primary_dataset.value;
        report.channel_count = entry->channel_count;
        report.pack_id = entry->pack_id.value;
        report.payload_offset = entry->payload_offset;
        report.stored_bytes = entry->stored_bytes;
        report.effective_resolution_millimeters = entry->effective_resolution_millimeters;
        report.geometric_error_millimeters = entry->geometric_error_millimeters;
        report.materialized_child_mask = entry->materialized_child_mask;
        report.parent = key->parent();
        for (const TileIndexEntry& child : database.value().Children(*key)) {
            report.children.push_back(child.key);
        }
        if (tile.value().provenance()) {
            for (const ProvenancePaletteEntry& provenance : tile.value().provenance()->palette) {
                report.contributing_datasets.push_back(provenance.dataset_id);
            }
        }
        const auto quality = std::ranges::find_if(
            tile.value().channels(), [](const DecodedChannel& channel) {
                return channel.id() == ChannelId::quality;
            });
        if (quality != tile.value().channels().end()) {
            std::uint8_t flags = 0;
            for (const std::byte value : quality->bytes()) {
                flags = static_cast<std::uint8_t>(flags | std::to_integer<std::uint8_t>(value));
            }
            report.quality_flags = flags;
        }
        report.content_hash_prefix = bytes_to_hex(entry->content_hash_prefix);
        report.dependency_hash_prefix = bytes_to_hex(entry->dependency_hash_prefix);
    }
    return Result<InspectionReport>::success(std::move(report));
}

std::string format_report(const ScanReport& report, const bool json) {
    if (json) {
        std::string raster = "null";
        if (report.raster_details) {
            std::string members;
            for (std::size_t index = 0; index < report.artifact_members.size(); ++index) {
                if (index != 0) {
                    members.push_back(',');
                }
                const ArtifactMember& member = report.artifact_members[index];
                members += fmt::format(
                    "{{\"bytes\":{},\"name\":{},\"sha256\":{}}}",
                    member.bytes,
                    json_string(member.name),
                    json_string(member.sha256.to_hex()));
            }
            const RasterSourceDetails& details = *report.raster_details;
            const std::string representation = details.elevation_representation ==
                ElevationRepresentation::elevation_meters ? "elevation_meters" : "radius_meters";
            raster = fmt::format(
                "{{\"artifact_bundle_bytes\":{},\"artifact_bundle_sha256\":{},"
                "\"artifact_members\":[{}],\"center_elevation_meters\":{},"
                "\"data_type\":{},\"driver\":{},\"elevation_representation\":{},"
                "\"east_longitude_degrees\":{},\"height\":{},"
                "\"no_data_value\":{},\"north_latitude_degrees\":{},"
                "\"sample_offset\":{},\"sample_scale\":{},"
                "\"south_latitude_degrees\":{},\"west_longitude_degrees\":{},\"width\":{}}}",
                *report.artifact_bundle_bytes,
                json_string(report.artifact_bundle_sha256->to_hex()),
                members,
                optional_double_json(report.center_elevation_meters),
                json_string(details.data_type_name),
                json_string(details.driver_name),
                json_string(representation),
                details.footprint.east_longitude_degrees,
                details.height,
                double_json(details.no_data_value),
                details.footprint.north_latitude_degrees,
                details.sample_offset,
                details.sample_scale,
                details.footprint.south_latitude_degrees,
                details.footprint.west_longitude_degrees,
                details.width);
        }
        std::string sources;
        for (std::size_t index = 0; index < report.sources.size(); ++index) {
            if (index != 0) {
                sources.push_back(',');
            }
            const ScanSourceReport& source = report.sources[index];
            sources += fmt::format(
                "{{\"artifact_bundle_bytes\":{},\"artifact_bundle_sha256\":{},"
                "\"center_elevation_meters\":{},\"coverage_degrees\":{},"
                "\"dataset_id\":{},\"effective_resolution_meters\":{},"
                "\"fusion_policy\":{},\"priority\":{},\"quality_mapping\":{},"
                "\"quality_members\":[{}],\"raster_file_count\":{},\"role\":{},"
                "\"source_uri\":{},\"stable_key\":{},\"unsupported_quality_values\":[{}]}}",
                *source.artifact_bundle_bytes,
                json_string(source.artifact_bundle_sha256->to_hex()),
                optional_double_json(source.center_elevation_meters),
                bounds_json(source.raster_details->footprint),
                source.dataset_id.value,
                source.effective_resolution_meters,
                json_string(fusion_policy_name(source.fusion_policy)),
                source.priority,
                json_string(source.quality_mapping),
                string_array_json(source.quality_members),
                source.raster_file_count,
                json_string(source.role == RasterSourceRole::base ? "base" : "refinement"),
                json_string(source.source_uri),
                json_string(source.stable_key),
                string_array_json(source.unsupported_quality_values));
        }
        const std::string region = report.required_region
            ? bounds_json(*report.required_region) : "null";
        return fmt::format(
            "{{\"builder_configuration_sha256\":{},\"database_name\":{},\"dataset_id\":{},"
            "\"raster\":{},\"required_region_degrees\":{},"
            "\"semantic_configuration_sha256\":{},\"source_registry\":[{}],\"source_uri\":{},"
            "\"stable_key\":{}}}\n",
            json_string(report.builder_configuration_hash.to_hex()),
            json_string(report.database_name),
            report.dataset_id.value,
            raster,
            region,
            json_string(report.semantic_configuration_hash.to_hex()),
            sources,
            json_string(report.source_uri),
            json_string(report.stable_key));
    }
    std::string text = fmt::format(
        "database: {}\ndataset: {} ({})\nsource: {}\nbuilder configuration sha256: {}\n"
        "semantic configuration sha256: {}\n",
        report.database_name,
        report.stable_key,
        report.dataset_id.value,
        report.source_uri,
        report.builder_configuration_hash.to_hex(),
        report.semantic_configuration_hash.to_hex());
    if (report.raster_details) {
        const RasterSourceDetails& details = *report.raster_details;
        text += fmt::format(
            "driver: {} ({})\nraster: {} x {}\ncoverage: {}..{} degrees east, {}..{} degrees north\n"
            "no-data: {}\nsample scale/offset: {} / {}\nartifact bundle: {} bytes, {}\n"
            "center elevation: {} m\n",
            details.driver_name,
            details.data_type_name,
            details.width,
            details.height,
            details.footprint.west_longitude_degrees,
            details.footprint.east_longitude_degrees,
            details.footprint.south_latitude_degrees,
            details.footprint.north_latitude_degrees,
            details.no_data_value,
            details.sample_scale,
            details.sample_offset,
            *report.artifact_bundle_bytes,
            report.artifact_bundle_sha256->to_hex(),
            report.center_elevation_meters.value_or(std::numeric_limits<double>::quiet_NaN()));
    }
    if (report.required_region) {
        text += fmt::format(
            "required region: {}..{} degrees east, {}..{} degrees north\n",
            report.required_region->west_longitude_degrees,
            report.required_region->east_longitude_degrees,
            report.required_region->south_latitude_degrees,
            report.required_region->north_latitude_degrees);
    }
    if (!report.sources.empty()) {
        text += "source registry (fusion order):\n";
        for (const ScanSourceReport& source : report.sources) {
            const GeographicBounds& coverage = source.raster_details->footprint;
            text += fmt::format(
                "  {} ({}) priority={} role={} policy={} files={} effective={} m "
                "coverage={}..{} E, {}..{} N\n"
                "    artifact bundle: {} bytes, {}\n"
                "    quality mapping: {}; unsupported: {}\n",
                source.stable_key,
                source.dataset_id.value,
                source.priority,
                source.role == RasterSourceRole::base ? "base" : "refinement",
                fusion_policy_name(source.fusion_policy),
                source.raster_file_count,
                source.effective_resolution_meters,
                coverage.west_longitude_degrees,
                coverage.east_longitude_degrees,
                coverage.south_latitude_degrees,
                coverage.north_latitude_degrees,
                *source.artifact_bundle_bytes,
                source.artifact_bundle_sha256->to_hex(),
                source.quality_mapping.empty() ? "none" : source.quality_mapping,
                source.unsupported_quality_values.empty()
                    ? "none" : fmt::format("{} value(s)", source.unsupported_quality_values.size()));
        }
    }
    return text;
}

std::string format_report(const PlanReport& report, const bool json) {
    if (json) {
        std::string tiles;
        const std::size_t listed_tiles = std::min<std::size_t>(
            report.expected_hierarchy_tiles.size(), 256U);
        for (std::size_t index = 0; index < listed_tiles; ++index) {
            if (index > 0) {
                tiles.push_back(',');
            }
            tiles += json_string(report.expected_hierarchy_tiles[index].to_string());
        }
        std::string prototype_tiles;
        for (std::size_t index = 0; index < report.tiles.size(); ++index) {
            if (index != 0) {
                prototype_tiles.push_back(',');
            }
            prototype_tiles += json_string(report.tiles[index].to_string());
        }
        std::string sources;
        for (std::size_t index = 0; index < report.sources.size(); ++index) {
            if (index != 0) {
                sources.push_back(',');
            }
            const PlanSourceReport& source = report.sources[index];
            sources += fmt::format(
                "{{\"coverage_degrees\":{},\"dataset_id\":{},"
                "\"effective_resolution_meters\":{},\"fusion_policy\":{},"
                "\"priority\":{},\"stable_key\":{},\"target_level\":{}}}",
                bounds_json(source.coverage),
                source.dataset_id.value,
                source.effective_resolution_meters,
                json_string(fusion_policy_name(source.fusion_policy)),
                source.priority,
                json_string(source.stable_key),
                source.target_level);
        }
        std::string levels;
        for (std::size_t index = 0; index < report.level_counts.size(); ++index) {
            if (index != 0) {
                levels.push_back(',');
            }
            levels += fmt::format(
                "{{\"level\":{},\"tile_count\":{}}}",
                report.level_counts[index].level,
                report.level_counts[index].tile_count);
        }
        return fmt::format(
            "{{\"estimated_uncompressed_channel_bytes\":{},\"level_counts\":[{}],"
            "\"prototype_tile_count\":{},\"prototype_tiles\":[{}],"
            "\"required_region_degrees\":{},\"sources\":[{}],"
            "\"tile_count\":{},\"tiles\":[{}],\"tiles_omitted\":{}}}\n",
            report.estimated_uncompressed_channel_bytes,
            levels,
            report.tiles.size(),
            prototype_tiles,
            report.required_region ? bounds_json(*report.required_region) : "null",
            sources,
            report.expected_hierarchy_tiles.size(),
            tiles,
            report.expected_hierarchy_tiles.size() - listed_tiles);
    }
    std::string text = fmt::format(
        "planned tiles: {}\nestimated uncompressed channel bytes: {}\n",
        report.expected_hierarchy_tiles.size(),
        report.estimated_uncompressed_channel_bytes);
    if (report.required_region) {
        text += fmt::format(
            "required region: {}..{} degrees east, {}..{} degrees north\n",
            report.required_region->west_longitude_degrees,
            report.required_region->east_longitude_degrees,
            report.required_region->south_latitude_degrees,
            report.required_region->north_latitude_degrees);
    }
    text += "source levels (fusion order):\n";
    for (const PlanSourceReport& source : report.sources) {
        text += fmt::format(
            "  {} ({}) priority={} policy={} effective={} m -> L{}; coverage={}..{} E, {}..{} N\n",
            source.stable_key,
            source.dataset_id.value,
            source.priority,
            fusion_policy_name(source.fusion_policy),
            source.effective_resolution_meters,
            source.target_level,
            source.coverage.west_longitude_degrees,
            source.coverage.east_longitude_degrees,
            source.coverage.south_latitude_degrees,
            source.coverage.north_latitude_degrees);
    }
    text += "hierarchy counts:\n";
    for (const PlanLevelCount count : report.level_counts) {
        text += fmt::format("  L{}: {}\n", count.level, count.tile_count);
    }
    text += "prototype tiles:\n";
    for (const LunarTileKey key : report.tiles) {
        text += fmt::format("  {}\n", key.to_string());
    }
    const std::size_t listed_tiles = std::min<std::size_t>(
        report.expected_hierarchy_tiles.size(), 256U);
    for (std::size_t index = 0; index < listed_tiles; ++index) {
        const LunarTileKey key = report.expected_hierarchy_tiles[index];
        text += fmt::format("  {}\n", key.to_string());
    }
    if (listed_tiles != report.expected_hierarchy_tiles.size()) {
        text += fmt::format(
            "  ... {} tile keys omitted\n",
            report.expected_hierarchy_tiles.size() - listed_tiles);
    }
    return text;
}

std::string format_report(const BuildReport& report, const bool json) {
    if (json) {
        std::string packs;
        for (std::size_t index = 0; index < report.packs.size(); ++index) {
            if (index > 0) {
                packs.push_back(',');
            }
            const PackBuildReport& pack = report.packs[index];
            packs += fmt::format(
                "{{\"bytes\":{},\"id\":{},\"path\":{},\"sha256\":{}}}",
                pack.bytes,
                pack.id.value,
                json_string(pack.path.string()),
                json_string(pack.sha256.to_hex()));
        }
        return fmt::format(
            "{{\"builder_configuration_sha256\":{},\"database_content_sha256\":{},"
            "\"database_path\":{},\"packs\":[{}],\"tile_count\":{},"
            "\"built_tile_count\":{},\"reused_tile_count\":{}}}\n",
            json_string(report.builder_configuration_hash.to_hex()),
            json_string(report.database_content_hash.to_hex()),
            json_string(report.database_path.string()),
            packs,
            report.tile_count,
            report.built_tile_count,
            report.reused_tile_count);
    }
    return fmt::format(
        "published: {}\ntiles: {} (built {}, reused {})\npacks: {}\n"
        "database content sha256: {}\n",
        report.database_path.string(),
        report.tile_count,
        report.built_tile_count,
        report.reused_tile_count,
        report.packs.size(),
        report.database_content_hash.to_hex());
}

std::string format_report(const ValidationReport& report, const bool json) {
    if (json) {
        return fmt::format(
            "{{\"database_path\":{},\"full\":{},\"pack_count\":{},\"status\":\"valid\","
            "\"tile_count\":{},\"verified_hierarchy_tiles\":{},"
            "\"verified_projection_samples\":{},\"verified_provenance_tiles\":{},"
            "\"verified_scientific_tiles\":{},\"verified_seams\":{}}}\n",
            json_string(report.database_path.string()),
            report.full,
            report.pack_count,
            report.tile_count,
            report.verified_hierarchy_tiles,
            report.verified_projection_samples,
            report.verified_provenance_tiles,
            report.verified_scientific_tiles,
            report.verified_seams);
    }
    return fmt::format(
        "valid: {}\nfull: {}\ntiles: {}\npacks: {}\nverified projection samples: {}\n"
        "verified scientific tiles: {}\nverified provenance tiles: {}\n"
        "verified hierarchy tiles: {}\nverified seams: {}\n",
        report.database_path.string(),
        report.full ? "yes" : "no",
        report.tile_count,
        report.pack_count,
        report.verified_projection_samples,
        report.verified_scientific_tiles,
        report.verified_provenance_tiles,
        report.verified_hierarchy_tiles,
        report.verified_seams);
}

std::string format_report(const InspectionReport& report, const bool json) {
    if (json) {
        std::string tile = "null";
        if (report.tile_key) {
            std::string children;
            for (std::size_t index = 0; index < report.children.size(); ++index) {
                if (index != 0) {
                    children.push_back(',');
                }
                children += json_string(report.children[index].to_string());
            }
            std::string datasets;
            for (std::size_t index = 0; index < report.contributing_datasets.size(); ++index) {
                if (index != 0) {
                    datasets.push_back(',');
                }
                datasets += fmt::format("{}", report.contributing_datasets[index].value);
            }
            const std::string parent = report.parent
                ? json_string(report.parent->to_string())
                : "null";
            const std::string quality = report.quality_flags
                ? fmt::format("{}", *report.quality_flags)
                : "null";
            tile = fmt::format(
                "{{\"channel_count\":{},\"children\":[{}],\"content_hash_prefix\":{},"
                "\"contributing_dataset_ids\":[{}],\"dependency_hash_prefix\":{},"
                "\"effective_resolution_millimeters\":{},\"geometric_error_millimeters\":{},"
                "\"key\":{},\"materialized_child_mask\":{},\"maximum_elevation_code\":{},"
                "\"minimum_elevation_code\":{},\"pack_id\":{},\"parent\":{},"
                "\"payload_offset\":{},\"primary_dataset_id\":{},\"quality_flags\":{},"
                "\"stored_bytes\":{}}}",
                *report.channel_count,
                children,
                json_string(report.content_hash_prefix),
                datasets,
                json_string(report.dependency_hash_prefix),
                *report.effective_resolution_millimeters,
                *report.geometric_error_millimeters,
                json_string(report.tile_key->to_string()),
                *report.materialized_child_mask,
                *report.maximum_elevation_code,
                *report.minimum_elevation_code,
                *report.pack_id,
                parent,
                *report.payload_offset,
                *report.primary_dataset_id,
                quality,
                *report.stored_bytes);
        }
        return fmt::format(
            "{{\"database_content_sha256\":{},\"database_path\":{},\"dataset_count\":{},"
            "\"pack_count\":{},\"tile\":{},\"tile_count\":{}}}\n",
            json_string(report.database_content_hash.to_hex()),
            json_string(report.database_path.string()),
            report.dataset_count,
            report.pack_count,
            tile,
            report.tile_count);
    }
    std::string text = fmt::format(
        "database: {}\ntiles: {}\ndatasets: {}\npacks: {}\ndatabase content sha256: {}\n",
        report.database_path.string(),
        report.tile_count,
        report.dataset_count,
        report.pack_count,
        report.database_content_hash.to_hex());
    if (report.tile_key) {
        text += fmt::format(
            "tile: {}\nparent: {}\nchildren: {}\nchannels: {}\nelevation codes: {}..{}\n"
            "primary dataset: {}\ncontributing datasets: {}\npack/offset/bytes: {} / {} / {}\n"
            "effective resolution: {} mm\ngeometric error: {} mm\nchild mask: {}\n"
            "content hash prefix: {}\ndependency hash prefix: {}\nquality flags: {}\n",
            report.tile_key->to_string(),
            report.parent ? report.parent->to_string() : "none",
            report.children.size(),
            *report.channel_count,
            *report.minimum_elevation_code,
            *report.maximum_elevation_code,
            *report.primary_dataset_id,
            report.contributing_datasets.size(),
            *report.pack_id,
            *report.payload_offset,
            *report.stored_bytes,
            *report.effective_resolution_millimeters,
            *report.geometric_error_millimeters,
            *report.materialized_child_mask,
            report.content_hash_prefix,
            report.dependency_hash_prefix,
            report.quality_flags ? fmt::format("{}", *report.quality_flags) : "none");
    }
    return text;
}

}  // namespace lunar::terrain::builder
