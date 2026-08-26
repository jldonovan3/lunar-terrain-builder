#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/raster_source.hpp"
#include "builder/tile_staging.hpp"

namespace lunar::terrain::builder {

inline constexpr double lunar_reference_radius_meters = 1'737'400.0;

struct HierarchySource {
    DatasetId dataset_id;
    GeographicFootprint footprint;
    double effective_resolution_meters{};
    std::uint8_t maximum_level{LunarTileKey::max_level};
};

struct HierarchySourceLevel {
    DatasetId dataset_id;
    std::uint8_t level{};
};

struct SparseHierarchyPlan {
    std::vector<LunarTileKey> tiles;
    // Parallel to tiles. Bits zero through three identify materialized direct
    // children in LunarTileKey::children() order.
    std::vector<std::uint8_t> child_masks;
    std::vector<HierarchySourceLevel> source_levels;
};

struct TileHierarchyMetadata {
    LunarTileKey key;
    double geometric_error_meters{};
    std::uint8_t materialized_child_mask{};
};

// Returns the largest measured center-to-neighbor spacing over a deterministic
// sampling of the source footprint at the requested QSC level.
[[nodiscard]] Result<double> worst_case_qsc_sample_spacing(
    const GeographicFootprint& footprint,
    std::uint8_t level,
    double reference_radius_meters = lunar_reference_radius_meters);

// Chooses the first level whose sampled worst-case spacing is no larger than
// the source's qualified effective resolution.
[[nodiscard]] Result<std::uint8_t> choose_source_level(
    const GeographicFootprint& footprint,
    double effective_resolution_meters,
    std::uint8_t maximum_level,
    double reference_radius_meters = lunar_reference_radius_meters);

// Plans intersecting target-level nodes and every direct ancestor. Output is
// unique and sorted by encoded TileKey.
[[nodiscard]] Result<SparseHierarchyPlan> plan_sparse_hierarchy(
    std::span<const HierarchySource> sources,
    double reference_radius_meters = lunar_reference_radius_meters);

// Validates uniqueness and direct-parent connectivity while deriving masks.
[[nodiscard]] Result<std::vector<std::uint8_t>> materialized_child_masks(
    std::span<const LunarTileKey> sorted_tiles);

// Finds the closest materialized parent. The supplied set need not be sorted.
[[nodiscard]] Result<LunarTileKey> nearest_materialized_ancestor(
    LunarTileKey descendant,
    std::span<const LunarTileKey> materialized_tiles);

// Frozen M6 reconstruction rule: bilinear interpolation of the nearest
// ancestor's quantized 257x257 core at every descendant core vertex. Returned
// error is the maximum absolute decoded elevation difference in meters.
[[nodiscard]] Result<double> geometric_error_bilinear_u16_v1(
    LunarTileKey descendant,
    std::span<const std::uint16_t> descendant_core,
    LunarTileKey ancestor,
    std::span<const std::uint16_t> ancestor_core);

// Derives child masks and nearest-ancestor errors for a canonical sorted set of
// finalized tiles. Roots have zero geometric error.
[[nodiscard]] Result<std::vector<TileHierarchyMetadata>> compute_hierarchy_metadata(
    std::span<const FinalizedElevationTile> sorted_tiles);

}  // namespace lunar::terrain::builder
