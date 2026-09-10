#include "builder/hierarchy.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_projection.hpp>

namespace lunar::terrain::builder {
namespace {

constexpr std::uint32_t footprint_samples_per_axis = 9;
constexpr std::uint32_t tile_bounds_samples_per_axis = 9;

[[nodiscard]] Error hierarchy_error(
    const ErrorCode code,
    std::string message,
    const std::optional<LunarTileKey> key = std::nullopt) {
    Error error{code, std::move(message)};
    if (key) {
        error.with_tile_key(key->encoded());
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::optional<LunarTileKey> key = std::nullopt) {
    return Result<T>::failure(hierarchy_error(code, std::move(message), key));
}

[[nodiscard]] bool valid_footprint(const GeographicFootprint& footprint) noexcept {
    return std::isfinite(footprint.west_longitude_degrees) &&
        std::isfinite(footprint.east_longitude_degrees) &&
        std::isfinite(footprint.south_latitude_degrees) &&
        std::isfinite(footprint.north_latitude_degrees) &&
        footprint.west_longitude_degrees >= -180.0 &&
        footprint.east_longitude_degrees <= 360.0 &&
        footprint.west_longitude_degrees < footprint.east_longitude_degrees &&
        footprint.south_latitude_degrees >= -90.0 &&
        footprint.north_latitude_degrees <= 90.0 &&
        footprint.south_latitude_degrees < footprint.north_latitude_degrees;
}

[[nodiscard]] double interpolate(
    const double low,
    const double high,
    const std::uint32_t index,
    const std::uint32_t count) noexcept {
    return low + (high - low) * static_cast<double>(index) /
        static_cast<double>(count - 1U);
}

[[nodiscard]] double radians(const double degrees) noexcept {
    return degrees * std::numbers::pi_v<double> / 180.0;
}

[[nodiscard]] double degrees(const double radians_value) noexcept {
    return radians_value * 180.0 / std::numbers::pi_v<double>;
}

[[nodiscard]] double normalized_longitude_for_footprint(
    double longitude_degrees,
    const GeographicFootprint& footprint) noexcept {
    const double center =
        (footprint.west_longitude_degrees + footprint.east_longitude_degrees) * 0.5;
    while (longitude_degrees - center > 180.0) {
        longitude_degrees -= 360.0;
    }
    while (longitude_degrees - center < -180.0) {
        longitude_degrees += 360.0;
    }
    return longitude_degrees;
}

[[nodiscard]] bool footprint_contains(
    const GeographicFootprint& footprint,
    const LunarGeodeticCoordinate coordinate) noexcept {
    const double longitude = normalized_longitude_for_footprint(
        degrees(coordinate.longitude_radians), footprint);
    const double latitude = degrees(coordinate.latitude_radians);
    return longitude >= footprint.west_longitude_degrees &&
        longitude <= footprint.east_longitude_degrees &&
        latitude >= footprint.south_latitude_degrees &&
        latitude <= footprint.north_latitude_degrees;
}

[[nodiscard]] double angular_distance(
    const LunarGeodeticCoordinate first,
    const LunarGeodeticCoordinate second) noexcept {
    const double half_latitude =
        (second.latitude_radians - first.latitude_radians) * 0.5;
    const double half_longitude =
        (second.longitude_radians - first.longitude_radians) * 0.5;
    const double latitude_sine = std::sin(half_latitude);
    const double longitude_sine = std::sin(half_longitude);
    const double haversine = latitude_sine * latitude_sine +
        std::cos(first.latitude_radians) * std::cos(second.latitude_radians) *
            longitude_sine * longitude_sine;
    return 2.0 * std::asin(std::sqrt(std::clamp(haversine, 0.0, 1.0)));
}

[[nodiscard]] Result<double> local_spacing(
    const LunarGeodeticCoordinate coordinate,
    const std::uint8_t level,
    const double reference_radius_meters) {
    auto qsc = QscProjection::Forward(coordinate);
    if (!qsc) {
        return Result<double>::failure(std::move(qsc).error());
    }
    const double denominator = static_cast<double>(
        std::uint64_t{format_v1::tile_cells} << level);
    const double step = 2.0 / denominator;
    double maximum = 0.0;
    for (const bool along_u : {true, false}) {
        QscCoordinate neighbor = qsc.value();
        double& component = along_u ? neighbor.u : neighbor.v;
        component += component + step <= 1.0 ? step : -step;
        auto inverse = QscProjection::Inverse(neighbor);
        if (!inverse) {
            return Result<double>::failure(std::move(inverse).error());
        }
        maximum = std::max(
            maximum,
            reference_radius_meters * angular_distance(coordinate, inverse.value()));
    }
    return Result<double>::success(maximum);
}

[[nodiscard]] Result<bool> tile_intersects_footprint(
    const LunarTileKey key,
    const GeographicFootprint& footprint) {
    double minimum_longitude = std::numeric_limits<double>::infinity();
    double maximum_longitude = -std::numeric_limits<double>::infinity();
    double minimum_latitude = std::numeric_limits<double>::infinity();
    double maximum_latitude = -std::numeric_limits<double>::infinity();

    for (std::uint32_t y = 0; y < tile_bounds_samples_per_axis; ++y) {
        const std::uint16_t local_y = static_cast<std::uint16_t>(
            y * format_v1::tile_cells / (tile_bounds_samples_per_axis - 1U));
        for (std::uint32_t x = 0; x < tile_bounds_samples_per_axis; ++x) {
            const std::uint16_t local_x = static_cast<std::uint16_t>(
                x * format_v1::tile_cells / (tile_bounds_samples_per_axis - 1U));
            auto u = QscProjection::LatticeCoordinate(key.x(), local_x, key.level());
            auto v = QscProjection::LatticeCoordinate(key.y(), local_y, key.level());
            if (!u || !v) {
                return Result<bool>::failure(u ? std::move(v).error() : std::move(u).error());
            }
            auto coordinate = QscProjection::Inverse(QscCoordinate{
                static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
            if (!coordinate) {
                return Result<bool>::failure(std::move(coordinate).error());
            }
            if (footprint_contains(footprint, coordinate.value())) {
                return Result<bool>::success(true);
            }
            const double longitude = normalized_longitude_for_footprint(
                degrees(coordinate.value().longitude_radians), footprint);
            const double latitude = degrees(coordinate.value().latitude_radians);
            minimum_longitude = std::min(minimum_longitude, longitude);
            maximum_longitude = std::max(maximum_longitude, longitude);
            minimum_latitude = std::min(minimum_latitude, latitude);
            maximum_latitude = std::max(maximum_latitude, latitude);
        }
    }

    // The sampled bounds are deliberately conservative for the curvilinear QSC
    // tile. Footprint points are also tested below so a small source wholly
    // inside a large tile is never missed.
    const bool bounds_overlap =
        maximum_longitude >= footprint.west_longitude_degrees &&
        minimum_longitude <= footprint.east_longitude_degrees &&
        maximum_latitude >= footprint.south_latitude_degrees &&
        minimum_latitude <= footprint.north_latitude_degrees;
    if (!bounds_overlap) {
        return Result<bool>::success(false);
    }

    const double tile_scale = static_cast<double>(std::uint64_t{1} << key.level());
    const double minimum_u = -1.0 + 2.0 * static_cast<double>(key.x()) / tile_scale;
    const double maximum_u = -1.0 + 2.0 * static_cast<double>(key.x() + 1U) / tile_scale;
    const double minimum_v = -1.0 + 2.0 * static_cast<double>(key.y()) / tile_scale;
    const double maximum_v = -1.0 + 2.0 * static_cast<double>(key.y() + 1U) / tile_scale;
    for (std::uint32_t y = 0; y < footprint_samples_per_axis; ++y) {
        for (std::uint32_t x = 0; x < footprint_samples_per_axis; ++x) {
            auto qsc = QscProjection::Forward(LunarGeodeticCoordinate{
                radians(interpolate(
                    footprint.south_latitude_degrees,
                    footprint.north_latitude_degrees,
                    y,
                    footprint_samples_per_axis)),
                radians(interpolate(
                    footprint.west_longitude_degrees,
                    footprint.east_longitude_degrees,
                    x,
                    footprint_samples_per_axis)),
                0.0,
            });
            if (!qsc) {
                return Result<bool>::failure(std::move(qsc).error());
            }
            if (static_cast<std::uint8_t>(qsc.value().face) == key.face() &&
                qsc.value().u >= minimum_u && qsc.value().u <= maximum_u &&
                qsc.value().v >= minimum_v && qsc.value().v <= maximum_v) {
                return Result<bool>::success(true);
            }
        }
    }
    return Result<bool>::success(bounds_overlap);
}

[[nodiscard]] Result<void> add_intersecting_nodes(
    const LunarTileKey key,
    const GeographicFootprint& footprint,
    const std::uint8_t target_level,
    std::set<LunarTileKey>& tiles) {
    auto intersects = tile_intersects_footprint(key, footprint);
    if (!intersects) {
        return Result<void>::failure(std::move(intersects).error());
    }
    if (!intersects.value()) {
        return Result<void>::success();
    }
    tiles.insert(key);
    if (key.level() == target_level) {
        return Result<void>::success();
    }
    auto children = key.children();
    if (!children) {
        return Result<void>::failure(std::move(children).error());
    }
    for (const LunarTileKey child : children.value()) {
        auto added = add_intersecting_nodes(child, footprint, target_level, tiles);
        if (!added) {
            return added;
        }
    }
    return Result<void>::success();
}

[[nodiscard]] constexpr std::size_t core_index(
    const std::uint32_t x,
    const std::uint32_t y) noexcept {
    return static_cast<std::size_t>(y) * format_v1::core_vertices + x;
}

}  // namespace

Result<double> worst_case_qsc_sample_spacing(
    const GeographicFootprint& footprint,
    const std::uint8_t level,
    const double reference_radius_meters) {
    if (!valid_footprint(footprint) || level > LunarTileKey::max_level ||
        !std::isfinite(reference_radius_meters) || reference_radius_meters <= 0.0) {
        return failure<double>(
            ErrorCode::invalid_argument,
            "QSC spacing requires a valid footprint, level, and reference radius");
    }
    double maximum = 0.0;
    for (std::uint32_t y = 0; y < footprint_samples_per_axis; ++y) {
        for (std::uint32_t x = 0; x < footprint_samples_per_axis; ++x) {
            const LunarGeodeticCoordinate coordinate{
                radians(interpolate(
                    footprint.south_latitude_degrees,
                    footprint.north_latitude_degrees,
                    y,
                    footprint_samples_per_axis)),
                radians(interpolate(
                    footprint.west_longitude_degrees,
                    footprint.east_longitude_degrees,
                    x,
                    footprint_samples_per_axis)),
                0.0,
            };
            auto spacing = local_spacing(coordinate, level, reference_radius_meters);
            if (!spacing) {
                return Result<double>::failure(std::move(spacing).error());
            }
            maximum = std::max(maximum, spacing.value());
        }
    }
    return Result<double>::success(maximum);
}

Result<std::uint8_t> choose_source_level(
    const GeographicFootprint& footprint,
    const double effective_resolution_meters,
    const std::uint8_t maximum_level,
    const double reference_radius_meters) {
    if (!std::isfinite(effective_resolution_meters) || effective_resolution_meters <= 0.0 ||
        maximum_level > LunarTileKey::max_level) {
        return failure<std::uint8_t>(
            ErrorCode::invalid_argument,
            "source level selection requires a positive effective resolution and valid maximum level");
    }
    for (std::uint8_t level = 0; level <= maximum_level; ++level) {
        auto spacing = worst_case_qsc_sample_spacing(footprint, level, reference_radius_meters);
        if (!spacing) {
            return Result<std::uint8_t>::failure(std::move(spacing).error());
        }
        if (spacing.value() <= effective_resolution_meters) {
            return Result<std::uint8_t>::success(level);
        }
    }
    return failure<std::uint8_t>(
        ErrorCode::invalid_argument,
        "source effective resolution requires a QSC level above tiles.max_level");
}

Result<bool> tile_intersects_source_footprint(
    const LunarTileKey key,
    const GeographicFootprint& footprint) {
    if (!valid_footprint(footprint)) {
        return failure<bool>(
            ErrorCode::invalid_argument,
            "tile/source intersection requires a valid geographic footprint",
            key);
    }
    return tile_intersects_footprint(key, footprint);
}

Result<SparseHierarchyPlan> plan_sparse_hierarchy(
    const std::span<const HierarchySource> sources,
    const double reference_radius_meters) {
    if (sources.empty()) {
        return failure<SparseHierarchyPlan>(
            ErrorCode::invalid_argument, "sparse hierarchy requires at least one source");
    }
    std::set<LunarTileKey> tiles;
    SparseHierarchyPlan plan;
    plan.source_levels.reserve(sources.size());
    for (const HierarchySource& source : sources) {
        auto level = choose_source_level(
            source.footprint,
            source.effective_resolution_meters,
            source.maximum_level,
            reference_radius_meters);
        if (!level) {
            return Result<SparseHierarchyPlan>::failure(std::move(level).error());
        }
        plan.source_levels.push_back(HierarchySourceLevel{source.dataset_id, level.value()});
        for (std::uint8_t face = 0; face <= LunarTileKey::max_face; ++face) {
            auto root = LunarTileKey::create(face, 0, 0, 0);
            if (!root) {
                return Result<SparseHierarchyPlan>::failure(std::move(root).error());
            }
            auto added = add_intersecting_nodes(
                root.value(), source.footprint, level.value(), tiles);
            if (!added) {
                return Result<SparseHierarchyPlan>::failure(std::move(added).error());
            }
        }
    }
    std::ranges::sort(plan.source_levels, {}, [](const HierarchySourceLevel& source) {
        return source.dataset_id.value;
    });
    plan.tiles.assign(tiles.begin(), tiles.end());
    auto masks = materialized_child_masks(plan.tiles);
    if (!masks) {
        return Result<SparseHierarchyPlan>::failure(std::move(masks).error());
    }
    plan.child_masks = std::move(masks).value();
    return Result<SparseHierarchyPlan>::success(std::move(plan));
}

Result<std::vector<std::uint8_t>> materialized_child_masks(
    const std::span<const LunarTileKey> sorted_tiles) {
    std::map<std::uint64_t, std::size_t> by_key;
    for (std::size_t index = 0; index < sorted_tiles.size(); ++index) {
        if (index != 0 && sorted_tiles[index] < sorted_tiles[index - 1U]) {
            return failure<std::vector<std::uint8_t>>(
                ErrorCode::invalid_argument, "materialized TileKeys must be sorted");
        }
        if (!by_key.emplace(sorted_tiles[index].encoded(), index).second) {
            return failure<std::vector<std::uint8_t>>(
                ErrorCode::invalid_argument,
                "materialized hierarchy contains a duplicate TileKey",
                sorted_tiles[index]);
        }
    }
    std::vector<std::uint8_t> masks(sorted_tiles.size(), 0);
    for (std::size_t index = 0; index < sorted_tiles.size(); ++index) {
        const LunarTileKey key = sorted_tiles[index];
        const std::optional<LunarTileKey> parent = key.parent();
        if (!parent) {
            continue;
        }
        const auto found = by_key.find(parent->encoded());
        if (found == by_key.end()) {
            return failure<std::vector<std::uint8_t>>(
                ErrorCode::invalid_argument,
                "materialized hierarchy contains an orphan tile",
                key);
        }
        const std::uint8_t quadrant = static_cast<std::uint8_t>(
            (key.x() & 1U) | ((key.y() & 1U) << 1U));
        masks[found->second] = static_cast<std::uint8_t>(
            masks[found->second] | static_cast<std::uint8_t>(1U << quadrant));
    }
    return Result<std::vector<std::uint8_t>>::success(std::move(masks));
}

Result<LunarTileKey> nearest_materialized_ancestor(
    const LunarTileKey descendant,
    const std::span<const LunarTileKey> materialized_tiles) {
    std::set<std::uint64_t> keys;
    for (const LunarTileKey key : materialized_tiles) {
        keys.insert(key.encoded());
    }
    std::optional<LunarTileKey> candidate = descendant.parent();
    while (candidate) {
        if (keys.contains(candidate->encoded())) {
            return Result<LunarTileKey>::success(*candidate);
        }
        candidate = candidate->parent();
    }
    return failure<LunarTileKey>(
        ErrorCode::not_found,
        "tile has no materialized ancestor",
        descendant);
}

Result<double> geometric_error_bilinear_u16_v1(
    const LunarTileKey descendant,
    const std::span<const std::uint16_t> descendant_core,
    const LunarTileKey ancestor,
    const std::span<const std::uint16_t> ancestor_core) {
    constexpr std::size_t sample_count =
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
    if (descendant_core.size() != sample_count || ancestor_core.size() != sample_count ||
        descendant.face() != ancestor.face() || ancestor.level() >= descendant.level()) {
        return failure<double>(
            ErrorCode::invalid_argument,
            "geometric error requires 257x257 cores and a strict same-face ancestor",
            descendant);
    }
    LunarTileKey current = descendant;
    while (current.level() > ancestor.level()) {
        const auto parent = current.parent();
        if (!parent) {
            return failure<double>(
                ErrorCode::invalid_argument,
                "geometric error ancestor does not contain the descendant",
                descendant);
        }
        current = *parent;
    }
    if (current != ancestor) {
        return failure<double>(
            ErrorCode::invalid_argument,
            "geometric error ancestor does not contain the descendant",
            descendant);
    }

    const std::uint8_t level_delta = static_cast<std::uint8_t>(
        descendant.level() - ancestor.level());
    const std::uint64_t scale = std::uint64_t{1} << level_delta;
    const std::uint64_t descendant_origin_x =
        std::uint64_t{descendant.x()} * format_v1::tile_cells;
    const std::uint64_t descendant_origin_y =
        std::uint64_t{descendant.y()} * format_v1::tile_cells;
    const std::uint64_t ancestor_origin_x =
        std::uint64_t{ancestor.x()} * format_v1::tile_cells;
    const std::uint64_t ancestor_origin_y =
        std::uint64_t{ancestor.y()} * format_v1::tile_cells;

    double maximum_code_error = 0.0;
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        const std::uint64_t numerator_y = descendant_origin_y + y;
        const std::uint64_t ancestor_global_y = numerator_y / scale;
        const std::uint64_t remainder_y = numerator_y % scale;
        const std::uint32_t y0 = static_cast<std::uint32_t>(
            ancestor_global_y - ancestor_origin_y);
        const std::uint32_t y1 = std::min<std::uint32_t>(
            y0 + 1U, format_v1::core_vertices - 1U);
        const double fy = static_cast<double>(remainder_y) / static_cast<double>(scale);
        for (std::uint32_t x = 0; x < format_v1::core_vertices; ++x) {
            const std::uint64_t numerator_x = descendant_origin_x + x;
            const std::uint64_t ancestor_global_x = numerator_x / scale;
            const std::uint64_t remainder_x = numerator_x % scale;
            const std::uint32_t x0 = static_cast<std::uint32_t>(
                ancestor_global_x - ancestor_origin_x);
            const std::uint32_t x1 = std::min<std::uint32_t>(
                x0 + 1U, format_v1::core_vertices - 1U);
            const double fx = static_cast<double>(remainder_x) / static_cast<double>(scale);

            const double south =
                static_cast<double>(ancestor_core[core_index(x0, y0)]) * (1.0 - fx) +
                static_cast<double>(ancestor_core[core_index(x1, y0)]) * fx;
            const double north =
                static_cast<double>(ancestor_core[core_index(x0, y1)]) * (1.0 - fx) +
                static_cast<double>(ancestor_core[core_index(x1, y1)]) * fx;
            const double reconstructed = south * (1.0 - fy) + north * fy;
            maximum_code_error = std::max(
                maximum_code_error,
                std::abs(static_cast<double>(descendant_core[core_index(x, y)]) -
                         reconstructed));
        }
    }
    return Result<double>::success(maximum_code_error * 0.5);
}

Result<std::vector<TileHierarchyMetadata>> compute_hierarchy_metadata(
    const std::span<const FinalizedElevationTile> sorted_tiles) {
    std::vector<LunarTileKey> keys;
    keys.reserve(sorted_tiles.size());
    std::map<std::uint64_t, std::size_t> by_key;
    for (std::size_t index = 0; index < sorted_tiles.size(); ++index) {
        keys.push_back(sorted_tiles[index].key);
        by_key.emplace(sorted_tiles[index].key.encoded(), index);
    }
    auto masks = materialized_child_masks(keys);
    if (!masks) {
        return Result<std::vector<TileHierarchyMetadata>>::failure(std::move(masks).error());
    }

    std::vector<TileHierarchyMetadata> metadata;
    metadata.reserve(sorted_tiles.size());
    for (std::size_t index = 0; index < sorted_tiles.size(); ++index) {
        const FinalizedElevationTile& tile = sorted_tiles[index];
        double geometric_error = 0.0;
        if (tile.key.level() != 0) {
            auto ancestor = nearest_materialized_ancestor(tile.key, keys);
            if (!ancestor) {
                return Result<std::vector<TileHierarchyMetadata>>::failure(
                    std::move(ancestor).error());
            }
            const auto found = by_key.find(ancestor.value().encoded());
            if (found == by_key.end()) {
                return failure<std::vector<TileHierarchyMetadata>>(
                    ErrorCode::internal_error,
                    "nearest materialized ancestor lookup became inconsistent",
                    tile.key);
            }
            auto error = geometric_error_bilinear_u16_v1(
                tile.key,
                tile.quantized_core,
                ancestor.value(),
                sorted_tiles[found->second].quantized_core);
            if (!error) {
                return Result<std::vector<TileHierarchyMetadata>>::failure(
                    std::move(error).error());
            }
            geometric_error = error.value();
        }
        metadata.push_back(TileHierarchyMetadata{
            tile.key, geometric_error, masks.value()[index]});
    }
    return Result<std::vector<TileHierarchyMetadata>>::success(std::move(metadata));
}

}  // namespace lunar::terrain::builder
