#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/hierarchy.hpp"

namespace lunar::terrain::builder {
namespace {

TEST_CASE("QSC source levels use worst-case local spacing") {
    const GeographicFootprint footprint{-1.0, 1.0, -1.0, 1.0};
    auto spacing7 = worst_case_qsc_sample_spacing(footprint, 7);
    auto spacing8 = worst_case_qsc_sample_spacing(footprint, 8);
    REQUIRE(spacing7);
    REQUIRE(spacing8);
    CHECK(spacing8.value() < spacing7.value());

    const double effective_resolution =
        (spacing7.value() + spacing8.value()) * 0.5;
    auto selected = choose_source_level(footprint, effective_resolution, 12);
    REQUIRE(selected);
    CHECK(selected.value() == 8);

    auto underresolved = choose_source_level(
        footprint, spacing8.value() * 0.5, 8);
    REQUIRE_FALSE(underresolved);
    CHECK(underresolved.error().code == ErrorCode::invalid_argument);
}

TEST_CASE("L8 L10 L12 source coverage forms a connected sparse hierarchy") {
    const GeographicFootprint broad{-0.03, 0.03, -0.03, 0.03};
    const GeographicFootprint regional{-0.012, 0.012, -0.012, 0.012};
    const GeographicFootprint local{-0.003, 0.003, -0.003, 0.003};

    auto broad7 = worst_case_qsc_sample_spacing(broad, 7);
    auto broad8 = worst_case_qsc_sample_spacing(broad, 8);
    auto regional9 = worst_case_qsc_sample_spacing(regional, 9);
    auto regional10 = worst_case_qsc_sample_spacing(regional, 10);
    auto local11 = worst_case_qsc_sample_spacing(local, 11);
    auto local12 = worst_case_qsc_sample_spacing(local, 12);
    REQUIRE(broad7);
    REQUIRE(broad8);
    REQUIRE(regional9);
    REQUIRE(regional10);
    REQUIRE(local11);
    REQUIRE(local12);

    const std::vector sources{
        HierarchySource{
            DatasetId{1}, broad, (broad7.value() + broad8.value()) * 0.5, 12},
        HierarchySource{
            DatasetId{2}, regional, (regional9.value() + regional10.value()) * 0.5, 12},
        HierarchySource{
            DatasetId{3}, local, (local11.value() + local12.value()) * 0.5, 12},
    };
    auto plan = plan_sparse_hierarchy(sources);
    REQUIRE(plan);
    REQUIRE(plan.value().source_levels.size() == 3);
    CHECK(plan.value().source_levels[0].level == 8);
    CHECK(plan.value().source_levels[1].level == 10);
    CHECK(plan.value().source_levels[2].level == 12);
    CHECK(std::ranges::is_sorted(plan.value().tiles));
    CHECK(plan.value().tiles.size() == plan.value().child_masks.size());
    CHECK(std::ranges::any_of(plan.value().tiles, [](const LunarTileKey key) {
        return key.level() == 8;
    }));
    CHECK(std::ranges::any_of(plan.value().tiles, [](const LunarTileKey key) {
        return key.level() == 10;
    }));
    CHECK(std::ranges::any_of(plan.value().tiles, [](const LunarTileKey key) {
        return key.level() == 12;
    }));

    auto masks = materialized_child_masks(plan.value().tiles);
    REQUIRE(masks);
    CHECK(masks.value() == plan.value().child_masks);
    for (const LunarTileKey key : plan.value().tiles) {
        if (key.level() == 0) {
            continue;
        }
        REQUIRE(key.parent());
        CHECK(std::ranges::binary_search(plan.value().tiles, *key.parent()));
    }
}

TEST_CASE("child masks reject orphaned and duplicate hierarchy nodes") {
    const auto root = LunarTileKey::create(0, 0, 0, 0).value();
    const auto children = root.children().value();
    auto masks = materialized_child_masks(std::vector{root, children[0], children[3]});
    REQUIRE(masks);
    CHECK(masks.value()[0] == 0b1001);

    auto orphaned = materialized_child_masks(std::vector{children[0]});
    REQUIRE_FALSE(orphaned);
    CHECK(orphaned.error().message.find("orphan") != std::string::npos);

    auto duplicated = materialized_child_masks(std::vector{root, root});
    REQUIRE_FALSE(duplicated);
    CHECK(duplicated.error().message.find("duplicate") != std::string::npos);
}

TEST_CASE("geometric error uses nearest-ancestor bilinear U16 reconstruction") {
    constexpr std::size_t count =
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
    std::vector<std::uint16_t> ancestor(count);
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::uint32_t x = 0; x < format_v1::core_vertices; ++x) {
            ancestor[std::size_t{y} * format_v1::core_vertices + x] =
                static_cast<std::uint16_t>(20'000U + x + y);
        }
    }
    const auto root = LunarTileKey::create(0, 0, 0, 0).value();
    const auto child = root.children().value()[3];
    std::vector<std::uint16_t> descendant(count);
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::uint32_t x = 0; x < format_v1::core_vertices; ++x) {
            // Northeast child maps to ancestor coordinates 128+x/2, 128+y/2.
            descendant[std::size_t{y} * format_v1::core_vertices + x] =
                static_cast<std::uint16_t>(20'000U + 256U + (x + y) / 2U);
        }
    }
    descendant[18] = static_cast<std::uint16_t>(descendant[18] + 7U);
    auto error = geometric_error_bilinear_u16_v1(child, descendant, root, ancestor);
    REQUIRE(error);
    CHECK(error.value() == Catch::Approx(3.5));

    const std::vector materialized{root, child};
    auto nearest = nearest_materialized_ancestor(child, materialized);
    REQUIRE(nearest);
    CHECK(nearest.value() == root);

    FinalizedElevationTile root_tile{root, {}, ancestor, {}};
    FinalizedElevationTile child_tile{child, {}, descendant, {}};
    auto metadata = compute_hierarchy_metadata(std::vector{root_tile, child_tile});
    REQUIRE(metadata);
    REQUIRE(metadata.value().size() == 2);
    CHECK(metadata.value()[0].materialized_child_mask == 0b1000);
    CHECK(metadata.value()[1].geometric_error_meters == Catch::Approx(3.5));
}

}  // namespace
}  // namespace lunar::terrain::builder
