#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <filesystem>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "builder/fusion.hpp"
#include "builder/tile_staging.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        path_ = std::filesystem::temp_directory_path() /
            ("lunar-terrain-fusion-tests-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] FusionGridSource constant_source(
    const DatasetId id,
    const std::int32_t priority,
    const FusionPolicy policy,
    const std::uint32_t width,
    const std::uint32_t height,
    const double resolution,
    const double value) {
    return FusionGridSource{
        id,
        priority,
        policy,
        resolution,
        width,
        height,
        std::vector<std::optional<double>>(std::size_t{width} * height, value),
        std::vector<std::uint8_t>(std::size_t{width} * height, 0),
    };
}

}  // namespace

TEST_CASE("M5 source ordering and residual refinement are deterministic") {
    constexpr std::uint32_t width = 97;
    constexpr std::uint32_t height = 9;
    FusionGridSource coarse = constant_source(
        DatasetId{40}, 100, FusionPolicy::replace, width, height, 80.0, 100.0);
    FusionGridSource fine = constant_source(
        DatasetId{20}, 200, FusionPolicy::residual_refinement_v1,
        width, height, 10.0, 0.0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::size_t index = std::size_t{y} * width + x;
            if (x < 8 || x >= width - 8) {
                fine.samples[index].reset();
            } else {
                const double broad = 130.0 + static_cast<double>(x) * 0.05;
                const double detail = (x % 2U == 0) ? 3.0 : -3.0;
                fine.samples[index] = broad + detail;
            }
        }
    }

    const std::vector first{fine, coarse};
    const std::vector second{coarse, fine};
    auto first_result = fuse_source_grids(first);
    auto second_result = fuse_source_grids(second);
    REQUIRE(first_result);
    REQUIRE(second_result);
    CHECK(first_result.value().elevations == second_result.value().elevations);
    CHECK(first_result.value().quality == second_result.value().quality);
    CHECK(first_result.value().contributions == second_result.value().contributions);

    const std::size_t boundary = 4U * width + 8U;
    const std::size_t interior_even = 4U * width + 48U;
    const std::size_t interior_odd = interior_even + 1U;
    CHECK(first_result.value().elevations[boundary] == 100.0);
    CHECK((first_result.value().quality[boundary] & quality_fusion_transition) != 0);
    CHECK(first_result.value().elevations[interior_even] > 101.0);
    CHECK(first_result.value().elevations[interior_odd] < 99.0);
    CHECK(std::abs(
        (first_result.value().elevations[interior_even] +
         first_result.value().elevations[interior_odd]) * 0.5 - 100.0) < 0.2);
    CHECK(residual_filter_pass_count(80.0, 10.0) == 3);
}

TEST_CASE("M5 replace and bias-corrected replace honor their contracts") {
    constexpr std::uint32_t width = 65;
    constexpr std::uint32_t height = 5;
    const FusionGridSource base = constant_source(
        DatasetId{1}, 0, FusionPolicy::replace, width, height, 100.0, 100.0);
    FusionGridSource replacement = constant_source(
        DatasetId{2}, 1, FusionPolicy::replace, width, height, 25.0, 120.0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            replacement.samples[std::size_t{y} * width + x] =
                120.0 + (x % 2U == 0 ? 2.0 : -2.0);
        }
    }

    auto replaced = fuse_source_grids(std::vector{base, replacement});
    REQUIRE(replaced);
    CHECK(replaced.value().elevations[32] == 122.0);

    replacement.policy = FusionPolicy::bias_corrected_replace;
    auto corrected = fuse_source_grids(std::vector{base, replacement});
    REQUIRE(corrected);
    CHECK(std::abs(corrected.value().elevations[32] - 102.0) < 0.1);
    CHECK((corrected.value().quality[32] & quality_bias_corrected) != 0);

    FusionGridSource same_priority_low = replacement;
    same_priority_low.dataset_id = DatasetId{10};
    same_priority_low.priority = 5;
    same_priority_low.policy = FusionPolicy::replace;
    std::ranges::fill(same_priority_low.samples, 130.0);
    FusionGridSource same_priority_high = same_priority_low;
    same_priority_high.dataset_id = DatasetId{20};
    std::ranges::fill(same_priority_high.samples, 140.0);
    auto id_ordered = fuse_source_grids(
        std::vector{same_priority_high, same_priority_low});
    REQUIRE(id_ordered);
    CHECK(id_ordered.value().elevations[32] == 140.0);
}

TEST_CASE("M5 transition derivatives remain continuous across a finalized tile seam") {
    constexpr std::uint32_t width = 513;
    constexpr std::uint32_t height = 257;
    FusionGridSource base = constant_source(
        DatasetId{1}, 0, FusionPolicy::replace, width, height, 100.0, 100.0);
    FusionGridSource fine = constant_source(
        DatasetId{2}, 1, FusionPolicy::replace, width, height, 25.0, 110.0);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < 240; ++x) {
            fine.samples[std::size_t{y} * width + x].reset();
        }
    }
    auto fused = fuse_source_grids(std::vector{fine, base});
    REQUIRE(fused);
    double maximum_first_derivative = 0.0;
    constexpr std::uint32_t row = 128;
    for (std::uint32_t x = 240; x < 274; ++x) {
        maximum_first_derivative = std::max(
            maximum_first_derivative,
            std::abs(
                fused.value().elevations[std::size_t{row} * width + x + 1U] -
                fused.value().elevations[std::size_t{row} * width + x]));
    }
    CHECK(maximum_first_derivative < 0.5);

    auto left_key = LunarTileKey::create(0, 1, 0, 0);
    auto right_key = LunarTileKey::create(0, 1, 1, 0);
    REQUIRE(left_key);
    REQUIRE(right_key);
    std::vector<double> left_samples;
    std::vector<double> right_samples;
    left_samples.reserve(std::size_t{257} * 257);
    right_samples.reserve(std::size_t{257} * 257);
    for (std::uint32_t y = 0; y < 257; ++y) {
        const auto row_begin = fused.value().elevations.begin() +
            static_cast<std::ptrdiff_t>(std::size_t{y} * width);
        left_samples.insert(left_samples.end(), row_begin, row_begin + 257);
        right_samples.insert(right_samples.end(), row_begin + 256, row_begin + 513);
    }
    TemporaryDirectory temporary;
    const Sha256Digest dependency{};
    auto left = stage_elevation_tile_samples(
        left_key.value(), dependency, temporary.path(), left_samples);
    auto right = stage_elevation_tile_samples(
        right_key.value(), dependency, temporary.path(), right_samples);
    REQUIRE(left);
    REQUIRE(right);
    std::vector staged{std::move(left).value(), std::move(right).value()};
    REQUIRE(resolve_elevation_boundaries(staged));
    const ElevationSampler apron_sampler = [](const QscCoordinate) {
        return Result<double>::success(100.0);
    };
    auto finalized = finalize_elevation_tiles(staged, apron_sampler);
    REQUIRE(finalized);
    for (std::uint32_t y = 0; y < 257; ++y) {
        CHECK(finalized.value()[0].quantized_core[std::size_t{y} * 257U + 256U] ==
              finalized.value()[1].quantized_core[std::size_t{y} * 257U]);
    }
}

TEST_CASE("M5 provenance and quality summaries are canonical") {
    constexpr std::uint32_t core = 257;
    FusionGridSource base = constant_source(
        DatasetId{99}, 0, FusionPolicy::replace, core, core, 100.0, 50.0);
    FusionGridSource fine = constant_source(
        DatasetId{12}, 1, FusionPolicy::replace, core, core, 10.0, 60.0);
    for (std::uint32_t y = 0; y < core; ++y) {
        for (std::uint32_t x = 0; x < core; ++x) {
            const std::size_t index = std::size_t{y} * core + x;
            if (x < 64) {
                fine.samples[index].reset();
            } else if (x == 64) {
                fine.quality[index] = quality_filled_no_data;
            }
        }
    }
    auto fused = fuse_source_grids(std::vector{fine, base});
    REQUIRE(fused);
    auto summary = summarize_fused_core(fused.value(), 0, 0);
    REQUIRE(summary);
    REQUIRE(summary.value().palette.size() == 2);
    CHECK(summary.value().palette[0].dataset_id == DatasetId{12});
    CHECK(summary.value().palette[1].dataset_id == DatasetId{99});
    CHECK(summary.value().primary_dataset == DatasetId{12});
    CHECK(summary.value().dominant_source_indices.size() == 64U * 64U);
    CHECK(summary.value().quality.size() == 64U * 64U);
    CHECK((summary.value().quality[16] & quality_fusion_transition) != 0);
    const double fraction_sum = summary.value().palette[0].contribution_fraction +
        summary.value().palette[1].contribution_fraction;
    CHECK(std::abs(fraction_sum - 1.0) < 1.0e-12);
}

}  // namespace lunar::terrain::builder
