#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>

#include "builder/configuration.hpp"

namespace lunar::terrain::builder {

inline constexpr std::uint8_t quality_interpolated = 1U << 0U;
inline constexpr std::uint8_t quality_filled_no_data = 1U << 1U;
inline constexpr std::uint8_t quality_fusion_transition = 1U << 2U;
inline constexpr std::uint8_t quality_bias_corrected = 1U << 3U;
inline constexpr std::uint8_t quality_lower_confidence = 1U << 4U;

struct FusionGridSource {
    DatasetId dataset_id;
    std::int32_t priority{};
    FusionPolicy policy{FusionPolicy::replace};
    double native_resolution_meters{};
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::optional<double>> samples;
    std::vector<std::uint8_t> quality;
};

struct FusedGrid {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<double> elevations;
    std::vector<std::uint8_t> quality;
    std::vector<DatasetId> source_ids;
    std::vector<double> source_resolutions_meters;
    // Sample-major contribution weights. Each sample owns source_ids.size()
    // consecutive binary64 weights in canonical DatasetID order.
    std::vector<double> contributions;
};

struct FusionPaletteEntry {
    DatasetId dataset_id;
    double contribution_fraction{};
    double native_resolution_meters{};
};

struct FusionTileSummary {
    DatasetId primary_dataset;
    std::vector<FusionPaletteEntry> palette;
    std::vector<std::uint16_t> dominant_source_indices;
    std::vector<std::uint8_t> quality;
};

// ResidualRefinement_v1 repeats the separable binomial filter once for equal
// resolutions and once more for each power-of-two increase in the coarse/fine
// resolution ratio.
[[nodiscard]] std::uint32_t residual_filter_pass_count(
    double coarse_resolution_meters,
    double fine_resolution_meters) noexcept;

// Sources are applied in ascending (priority, DatasetID) order independent of
// their input order. All arithmetic and reductions use fixed row-major order.
[[nodiscard]] Result<FusedGrid> fuse_source_grids(
    std::span<const FusionGridSource> sources);

// Extracts a 257x257 core from a fused grid and derives the canonical sorted
// palette, optional 64x64 dominant-source map, and optional 64x64 quality map.
[[nodiscard]] Result<FusionTileSummary> summarize_fused_core(
    const FusedGrid& grid,
    std::uint32_t origin_x,
    std::uint32_t origin_y);

}  // namespace lunar::terrain::builder
