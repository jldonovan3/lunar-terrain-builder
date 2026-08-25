#include "builder/builder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
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
#include <lunar/terrain/qsc_topology.hpp>

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
    const std::filesystem::path& path) {
    std::vector<TileIndexEntry> entries;
    for (std::uint8_t face = 0; face < 6; ++face) {
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
    for (const TileIndexEntry& entry : entries) {
        auto decoded = database.ReadTile(entry.key);
        if (!decoded) {
            return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::failure(
                std::move(decoded).error());
        }
        tiles.emplace_back(entry, std::move(decoded).value());
    }
    return Result<std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>>::success(
        std::move(tiles));
}

[[nodiscard]] Result<std::uint64_t> validate_seams(
    const std::vector<std::pair<TileIndexEntry, DecodedTerrainTile>>& tiles,
    const std::filesystem::path& path) {
    std::map<std::uint64_t, const DecodedTerrainTile*> by_key;
    for (const auto& [entry, tile] : tiles) {
        by_key.emplace(entry.key.encoded(), &tile);
    }

    constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    std::uint64_t verified = 0;
    for (const auto& [entry, tile] : tiles) {
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

}  // namespace

std::string_view version_string() noexcept {
    return "0.3.0";
}

Result<ScanReport> scan_configuration(const BuilderConfiguration& configuration) {
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
        auto sources = open_raster_sources(configuration, identity.value());
        if (!sources) {
            return Result<ScanReport>::failure(std::move(sources).error());
        }
        const IRasterSource& source = *sources.value()[representative];
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
        auto sample = source.Sample(LunarGeodeticCoordinate{
            latitude * std::numbers::pi_v<double> / 180.0,
            longitude * std::numbers::pi_v<double> / 180.0,
            0.0,
        });
        if (!sample) {
            return Result<ScanReport>::failure(std::move(sample).error());
        }
        report.center_elevation_meters = sample.value().elevation_meters;
    }
    return Result<ScanReport>::success(std::move(report));
}

Result<PlanReport> plan_configuration(const BuilderConfiguration& configuration) {
    if (configuration.source_kind == BuilderSourceKind::synthetic) {
        return plan_synthetic(configuration);
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<PlanReport>::failure(std::move(identity).error());
    }
    auto sources = open_raster_sources(configuration, identity.value());
    if (!sources) {
        return Result<PlanReport>::failure(std::move(sources).error());
    }
    std::size_t target = 0;
    for (std::size_t index = 1; index < configuration.rasters.size(); ++index) {
        if (std::tuple{configuration.rasters[target].priority,
                       identity.value().dataset_ids[target].value} <
            std::tuple{configuration.rasters[index].priority,
                       identity.value().dataset_ids[index].value}) {
            target = index;
        }
    }
    auto key = choose_raster_prototype_tile(
        *sources.value()[target], configuration.maximum_level);
    if (!key) {
        return Result<PlanReport>::failure(std::move(key).error());
    }
    constexpr std::uint64_t elevation_bytes =
        std::uint64_t{format_v1::serialized_elevation_samples} *
        format_v1::serialized_elevation_samples * 2U;
    const std::uint64_t provenance_bytes =
        format_v1::bytes::provenance_header +
        configuration.rasters.size() * format_v1::bytes::provenance_palette_entry +
        (configuration.rasters.size() > 1 ? 64U * 64U : 0U);
    const std::uint64_t quality_bytes = configuration.rasters.size() > 1 ? 64U * 64U : 0U;
    return Result<PlanReport>::success(PlanReport{
        {key.value()},
        elevation_bytes + provenance_bytes + quality_bytes,
    });
}

Result<BuildReport> build_configuration(const BuilderConfiguration& configuration) {
    return configuration.source_kind == BuilderSourceKind::synthetic
        ? build_synthetic(configuration)
        : build_raster_source(configuration);
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
    const bool full) {
    auto database = LunarTerrainDatabase::Open(path);
    if (!database) {
        return Result<ValidationReport>::failure(std::move(database).error());
    }
    auto tiles = read_all_tiles(database.value(), path);
    if (!tiles) {
        return Result<ValidationReport>::failure(std::move(tiles).error());
    }
    std::uint64_t seams = 0;
    if (full) {
        auto validated = validate_seams(tiles.value(), path);
        if (!validated) {
            return Result<ValidationReport>::failure(std::move(validated).error());
        }
        seams = validated.value();
    }
    return Result<ValidationReport>::success(ValidationReport{
        path,
        database.value().Header().tile_count,
        database.value().Header().pack_count,
        seams,
    });
}

Result<InspectionReport> inspect_database(
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key) {
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
                *report.center_elevation_meters,
                json_string(details.data_type_name),
                json_string(details.driver_name),
                json_string(representation),
                details.footprint.east_longitude_degrees,
                details.height,
                details.no_data_value,
                details.footprint.north_latitude_degrees,
                details.sample_offset,
                details.sample_scale,
                details.footprint.south_latitude_degrees,
                details.footprint.west_longitude_degrees,
                details.width);
        }
        return fmt::format(
            "{{\"builder_configuration_sha256\":{},\"database_name\":{},\"dataset_id\":{},"
            "\"raster\":{},\"semantic_configuration_sha256\":{},\"source_uri\":{},"
            "\"stable_key\":{}}}\n",
            json_string(report.builder_configuration_hash.to_hex()),
            json_string(report.database_name),
            report.dataset_id.value,
            raster,
            json_string(report.semantic_configuration_hash.to_hex()),
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
            *report.center_elevation_meters);
    }
    return text;
}

std::string format_report(const PlanReport& report, const bool json) {
    if (json) {
        std::string tiles;
        for (std::size_t index = 0; index < report.tiles.size(); ++index) {
            if (index > 0) {
                tiles.push_back(',');
            }
            tiles += json_string(report.tiles[index].to_string());
        }
        return fmt::format(
            "{{\"estimated_uncompressed_channel_bytes\":{},\"tile_count\":{},\"tiles\":[{}]}}\n",
            report.estimated_uncompressed_channel_bytes,
            report.tiles.size(),
            tiles);
    }
    std::string text = fmt::format(
        "planned tiles: {}\nestimated uncompressed channel bytes: {}\n",
        report.tiles.size(),
        report.estimated_uncompressed_channel_bytes);
    for (const LunarTileKey key : report.tiles) {
        text += fmt::format("  {}\n", key.to_string());
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
            "\"database_path\":{},\"packs\":[{}],\"tile_count\":{}}}\n",
            json_string(report.builder_configuration_hash.to_hex()),
            json_string(report.database_content_hash.to_hex()),
            json_string(report.database_path.string()),
            packs,
            report.tile_count);
    }
    return fmt::format(
        "published: {}\ntiles: {}\npacks: {}\ndatabase content sha256: {}\n",
        report.database_path.string(),
        report.tile_count,
        report.packs.size(),
        report.database_content_hash.to_hex());
}

std::string format_report(const ValidationReport& report, const bool json) {
    if (json) {
        return fmt::format(
            "{{\"database_path\":{},\"pack_count\":{},\"status\":\"valid\","
            "\"tile_count\":{},\"verified_seams\":{}}}\n",
            json_string(report.database_path.string()),
            report.pack_count,
            report.tile_count,
            report.verified_seams);
    }
    return fmt::format(
        "valid: {}\ntiles: {}\npacks: {}\nverified seams: {}\n",
        report.database_path.string(),
        report.tile_count,
        report.pack_count,
        report.verified_seams);
}

std::string format_report(const InspectionReport& report, const bool json) {
    if (json) {
        std::string tile = "null";
        if (report.tile_key) {
            tile = fmt::format(
                "{{\"channel_count\":{},\"key\":{},\"maximum_elevation_code\":{},"
                "\"minimum_elevation_code\":{},\"primary_dataset_id\":{}}}",
                *report.channel_count,
                json_string(report.tile_key->to_string()),
                *report.maximum_elevation_code,
                *report.minimum_elevation_code,
                *report.primary_dataset_id);
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
            "tile: {}\nchannels: {}\nelevation codes: {}..{}\nprimary dataset: {}\n",
            report.tile_key->to_string(),
            *report.channel_count,
            *report.minimum_elevation_code,
            *report.maximum_elevation_code,
            *report.primary_dataset_id);
    }
    return text;
}

}  // namespace lunar::terrain::builder
