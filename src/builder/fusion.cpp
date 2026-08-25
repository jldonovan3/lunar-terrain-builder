#include "builder/fusion.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <tuple>
#include <utility>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>

namespace lunar::terrain::builder {
namespace {

constexpr std::array<std::uint32_t, 5> binomial_weights{1, 4, 6, 4, 1};
constexpr std::uint32_t binomial_weight_sum = 16;
constexpr std::uint32_t minimum_valid_weight = binomial_weight_sum / 2;
constexpr std::uint32_t transition_width_samples = 32;

[[nodiscard]] Error fusion_error(std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)};
}

[[nodiscard]] constexpr std::size_t sample_index(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint32_t width) noexcept {
    return std::size_t{y} * width + x;
}

[[nodiscard]] std::vector<std::optional<double>> filter_axis(
    const std::vector<std::optional<double>>& input,
    const std::uint32_t width,
    const std::uint32_t height,
    const bool horizontal) {
    std::vector<std::optional<double>> output(input.size());
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            double weighted_sum = 0.0;
            std::uint32_t valid_weight = 0;
            for (std::int32_t tap = -2; tap <= 2; ++tap) {
                const std::int64_t sample_x = static_cast<std::int64_t>(x) + (horizontal ? tap : 0);
                const std::int64_t sample_y = static_cast<std::int64_t>(y) + (horizontal ? 0 : tap);
                if (sample_x < 0 || sample_y < 0 ||
                    sample_x >= static_cast<std::int64_t>(width) ||
                    sample_y >= static_cast<std::int64_t>(height)) {
                    continue;
                }
                const auto& value = input[sample_index(
                    static_cast<std::uint32_t>(sample_x),
                    static_cast<std::uint32_t>(sample_y),
                    width)];
                if (!value) {
                    continue;
                }
                const std::uint32_t weight = binomial_weights[static_cast<std::size_t>(tap + 2)];
                weighted_sum += *value * static_cast<double>(weight);
                valid_weight += weight;
            }
            if (valid_weight >= minimum_valid_weight) {
                output[sample_index(x, y, width)] =
                    weighted_sum / static_cast<double>(valid_weight);
            }
        }
    }
    return output;
}

[[nodiscard]] std::vector<std::optional<double>> low_pass(
    const FusionGridSource& source,
    const std::uint32_t passes) {
    std::vector<std::optional<double>> filtered = source.samples;
    for (std::size_t index = 0; index < filtered.size(); ++index) {
        if (!filtered[index] ||
            (!source.quality.empty() &&
             (source.quality[index] & quality_filled_no_data) != 0)) {
            filtered[index].reset();
        }
    }
    for (std::uint32_t pass = 0; pass < passes; ++pass) {
        filtered = filter_axis(filtered, source.width, source.height, true);
        filtered = filter_axis(filtered, source.width, source.height, false);
    }
    return filtered;
}

[[nodiscard]] std::vector<std::uint32_t> valid_boundary_distance(
    const FusionGridSource& source) {
    constexpr std::uint32_t far = std::numeric_limits<std::uint32_t>::max() / 4U;
    std::vector<std::uint32_t> distance(source.samples.size(), far);
    for (std::size_t index = 0; index < source.samples.size(); ++index) {
        if (!source.samples[index] ||
            (!source.quality.empty() &&
             (source.quality[index] & quality_filled_no_data) != 0)) {
            distance[index] = 0;
        }
    }
    for (std::uint32_t y = 0; y < source.height; ++y) {
        for (std::uint32_t x = 0; x < source.width; ++x) {
            const std::size_t index = sample_index(x, y, source.width);
            if (x > 0) {
                distance[index] = std::min(distance[index], distance[index - 1U] + 1U);
            }
            if (y > 0) {
                distance[index] = std::min(
                    distance[index], distance[index - source.width] + 1U);
            }
        }
    }
    for (std::uint32_t y = source.height; y-- > 0;) {
        for (std::uint32_t x = source.width; x-- > 0;) {
            const std::size_t index = sample_index(x, y, source.width);
            if (x + 1U < source.width) {
                distance[index] = std::min(distance[index], distance[index + 1U] + 1U);
            }
            if (y + 1U < source.height) {
                distance[index] = std::min(
                    distance[index], distance[index + source.width] + 1U);
            }
        }
    }
    return distance;
}

[[nodiscard]] double transition_weight(const std::uint32_t distance) noexcept {
    if (distance == 0) {
        return 0.0;
    }
    const double coordinate = std::clamp(
        static_cast<double>(distance - 1U) /
            static_cast<double>(transition_width_samples),
        0.0,
        1.0);
    return coordinate * coordinate * (3.0 - 2.0 * coordinate);
}

[[nodiscard]] double bias_correction(
    const FusionGridSource& source,
    const std::vector<bool>& current_valid,
    const std::vector<std::uint32_t>& boundary_distance,
    const std::vector<double>& current) noexcept {
    double sum = 0.0;
    std::uint64_t count = 0;
    for (std::size_t index = 0; index < source.samples.size(); ++index) {
        if (current_valid[index] && source.samples[index] && boundary_distance[index] != 0) {
            sum += *source.samples[index] - current[index];
            ++count;
        }
    }
    return count == 0 ? 0.0 : sum / static_cast<double>(count);
}

}  // namespace

std::uint32_t residual_filter_pass_count(
    const double coarse_resolution_meters,
    const double fine_resolution_meters) noexcept {
    if (!std::isfinite(coarse_resolution_meters) ||
        !std::isfinite(fine_resolution_meters) ||
        coarse_resolution_meters <= 0.0 || fine_resolution_meters <= 0.0) {
        return 0;
    }
    const double ratio = std::max(1.0, coarse_resolution_meters / fine_resolution_meters);
    return std::max<std::uint32_t>(
        1U,
        static_cast<std::uint32_t>(std::ceil(std::log2(ratio))));
}

Result<FusedGrid> fuse_source_grids(const std::span<const FusionGridSource> sources) {
    if (sources.empty()) {
        return Result<FusedGrid>::failure(fusion_error("fusion requires at least one source"));
    }
    const std::uint32_t width = sources.front().width;
    const std::uint32_t height = sources.front().height;
    if (width == 0 || height == 0 ||
        std::uint64_t{width} * height > std::numeric_limits<std::size_t>::max()) {
        return Result<FusedGrid>::failure(fusion_error("fusion grid dimensions are invalid"));
    }
    const std::size_t sample_count = std::size_t{width} * height;
    for (const FusionGridSource& source : sources) {
        if (source.width != width || source.height != height ||
            source.samples.size() != sample_count ||
            (!source.quality.empty() && source.quality.size() != sample_count) ||
            !std::isfinite(source.native_resolution_meters) ||
            source.native_resolution_meters <= 0.0) {
            return Result<FusedGrid>::failure(
                fusion_error("fusion source dimensions, quality, or resolution are invalid"));
        }
        for (const auto& sample : source.samples) {
            if (sample && !std::isfinite(*sample)) {
                return Result<FusedGrid>::failure(
                    fusion_error("fusion source contains a non-finite elevation"));
            }
        }
    }

    std::vector<std::size_t> order(sources.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, {}, [&sources](const std::size_t index) {
        return std::tuple{sources[index].priority, sources[index].dataset_id.value};
    });
    for (std::size_t index = 1; index < order.size(); ++index) {
        if (sources[order[index - 1U]].dataset_id == sources[order[index]].dataset_id) {
            return Result<FusedGrid>::failure(
                fusion_error("fusion DatasetIDs must be unique"));
        }
    }

    FusedGrid result;
    result.width = width;
    result.height = height;
    result.elevations.assign(sample_count, 0.0);
    result.quality.assign(sample_count, 0);
    result.source_ids.reserve(sources.size());
    result.source_resolutions_meters.reserve(sources.size());
    std::vector<std::size_t> contribution_index(sources.size());
    std::vector<std::size_t> dataset_order(sources.size());
    std::iota(dataset_order.begin(), dataset_order.end(), 0U);
    std::ranges::sort(dataset_order, {}, [&sources](const std::size_t index) {
        return sources[index].dataset_id.value;
    });
    for (std::size_t index = 0; index < dataset_order.size(); ++index) {
        contribution_index[dataset_order[index]] = index;
        result.source_ids.push_back(sources[dataset_order[index]].dataset_id);
        result.source_resolutions_meters.push_back(
            sources[dataset_order[index]].native_resolution_meters);
    }
    result.contributions.assign(sample_count * sources.size(), 0.0);

    std::vector<bool> current_valid(sample_count, false);
    double current_resolution = sources[order.front()].native_resolution_meters;
    for (const std::size_t source_index : order) {
        const FusionGridSource& source = sources[source_index];
        const auto distance = valid_boundary_distance(source);
        std::vector<std::optional<double>> filtered;
        if (source.policy == FusionPolicy::residual_refinement_v1) {
            const std::uint32_t passes = residual_filter_pass_count(
                current_resolution, source.native_resolution_meters);
            if (passes == 0 || passes > 64U) {
                return Result<FusedGrid>::failure(
                    fusion_error("residual filter pass count is outside the supported v1 range"));
            }
            filtered = low_pass(source, passes);
        }
        const double bias = source.policy == FusionPolicy::bias_corrected_replace
            ? bias_correction(source, current_valid, distance, result.elevations)
            : 0.0;
        bool source_contributed = false;
        for (std::size_t index = 0; index < sample_count; ++index) {
            if (!source.samples[index]) {
                continue;
            }
            const std::uint8_t source_quality = source.quality.empty() ? 0 : source.quality[index];
            const std::size_t source_weight_index =
                index * sources.size() + contribution_index[source_index];
            if (!current_valid[index]) {
                result.elevations[index] = *source.samples[index];
                result.quality[index] = source_quality;
                result.contributions[source_weight_index] = 1.0;
                current_valid[index] = true;
                source_contributed = true;
                continue;
            }

            double weight = transition_weight(distance[index]);
            double target = *source.samples[index];
            if (source.policy == FusionPolicy::bias_corrected_replace) {
                target -= bias;
            } else if (source.policy == FusionPolicy::residual_refinement_v1) {
                if (!filtered[index]) {
                    weight = 0.0;
                } else {
                    target = result.elevations[index] +
                        (*source.samples[index] - *filtered[index]);
                }
            }
            if (weight < 1.0) {
                result.quality[index] |= quality_fusion_transition;
            }
            if (source.policy == FusionPolicy::bias_corrected_replace && weight > 0.0) {
                result.quality[index] |= quality_bias_corrected;
            }
            result.quality[index] |= source_quality;
            if (weight == 0.0) {
                continue;
            }
            result.elevations[index] += weight * (target - result.elevations[index]);
            const double provenance_weight =
                source.policy == FusionPolicy::residual_refinement_v1
                ? weight * 0.5
                : weight;
            const std::size_t contribution_offset = index * sources.size();
            for (std::size_t contribution = 0; contribution < sources.size(); ++contribution) {
                result.contributions[contribution_offset + contribution] *=
                    (1.0 - provenance_weight);
            }
            result.contributions[source_weight_index] += provenance_weight;
            source_contributed = true;
        }
        if (source_contributed) {
            current_resolution = std::min(current_resolution, source.native_resolution_meters);
        }
    }
    if (std::ranges::any_of(current_valid, [](const bool valid) { return !valid; })) {
        return Result<FusedGrid>::failure(
            fusion_error("fusion sources leave uncovered output samples"));
    }
    return Result<FusedGrid>::success(std::move(result));
}

Result<FusionTileSummary> summarize_fused_core(
    const FusedGrid& grid,
    const std::uint32_t origin_x,
    const std::uint32_t origin_y) {
    constexpr std::uint32_t core = format_v1::core_vertices;
    constexpr std::uint32_t map_size = 64;
    constexpr std::uint32_t block = format_v1::tile_cells / map_size;
    if (grid.source_ids.empty() ||
        grid.source_ids.size() != grid.source_resolutions_meters.size() ||
        origin_x + core > grid.width || origin_y + core > grid.height ||
        grid.elevations.size() != std::size_t{grid.width} * grid.height ||
        grid.quality.size() != grid.elevations.size() ||
        grid.contributions.size() != grid.elevations.size() * grid.source_ids.size()) {
        return Result<FusionTileSummary>::failure(
            fusion_error("fused core summary input is inconsistent"));
    }

    std::vector<double> totals(grid.source_ids.size(), 0.0);
    for (std::uint32_t y = 0; y < core; ++y) {
        for (std::uint32_t x = 0; x < core; ++x) {
            const std::size_t source_sample = sample_index(origin_x + x, origin_y + y, grid.width);
            const std::size_t contribution_offset = source_sample * grid.source_ids.size();
            for (std::size_t source = 0; source < grid.source_ids.size(); ++source) {
                totals[source] += grid.contributions[contribution_offset + source];
            }
        }
    }

    FusionTileSummary summary;
    std::vector<std::size_t> palette_sources;
    const double denominator = static_cast<double>(std::size_t{core} * core);
    for (std::size_t source = 0; source < totals.size(); ++source) {
        if (totals[source] > 0.0) {
            palette_sources.push_back(source);
            summary.palette.push_back(FusionPaletteEntry{
                grid.source_ids[source],
                totals[source] / denominator,
                grid.source_resolutions_meters[source],
            });
        }
    }
    if (summary.palette.empty()) {
        return Result<FusionTileSummary>::failure(
            fusion_error("fused core has no provenance contributors"));
    }
    const auto primary = std::ranges::max_element(
        summary.palette,
        [](const FusionPaletteEntry& left, const FusionPaletteEntry& right) {
            if (left.contribution_fraction != right.contribution_fraction) {
                return left.contribution_fraction < right.contribution_fraction;
            }
            return left.dataset_id.value > right.dataset_id.value;
        });
    summary.primary_dataset = primary->dataset_id;

    if (summary.palette.size() > 1) {
        summary.dominant_source_indices.reserve(map_size * map_size);
        for (std::uint32_t map_y = 0; map_y < map_size; ++map_y) {
            for (std::uint32_t map_x = 0; map_x < map_size; ++map_x) {
                std::vector<double> block_totals(summary.palette.size(), 0.0);
                for (std::uint32_t y = 0; y < block; ++y) {
                    for (std::uint32_t x = 0; x < block; ++x) {
                        const std::size_t source_sample = sample_index(
                            origin_x + map_x * block + x,
                            origin_y + map_y * block + y,
                            grid.width);
                        const std::size_t contribution_offset =
                            source_sample * grid.source_ids.size();
                        for (std::size_t palette = 0; palette < palette_sources.size(); ++palette) {
                            block_totals[palette] += grid.contributions[
                                contribution_offset + palette_sources[palette]];
                        }
                    }
                }
                const auto dominant = std::ranges::max_element(block_totals);
                summary.dominant_source_indices.push_back(static_cast<std::uint16_t>(
                    std::distance(block_totals.begin(), dominant)));
            }
        }
    }

    std::vector<std::uint8_t> quality;
    quality.reserve(map_size * map_size);
    bool any_quality = false;
    for (std::uint32_t map_y = 0; map_y < map_size; ++map_y) {
        for (std::uint32_t map_x = 0; map_x < map_size; ++map_x) {
            std::uint8_t flags = 0;
            for (std::uint32_t y = 0; y < block; ++y) {
                for (std::uint32_t x = 0; x < block; ++x) {
                    flags |= grid.quality[sample_index(
                        origin_x + map_x * block + x,
                        origin_y + map_y * block + y,
                        grid.width)];
                }
            }
            flags &= 0x1FU;
            quality.push_back(flags);
            any_quality = any_quality || flags != 0;
        }
    }
    if (any_quality) {
        summary.quality = std::move(quality);
    }
    return Result<FusionTileSummary>::success(std::move(summary));
}

}  // namespace lunar::terrain::builder
