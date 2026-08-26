#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/qsc_topology.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/tile_staging.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("lunar-terrain-m4-" + std::to_string(suffix));
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

constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};

[[nodiscard]] constexpr std::size_t core_index(
    const std::uint16_t x,
    const std::uint16_t y) noexcept {
    return std::size_t{y} * format_v1::core_vertices + x;
}

[[nodiscard]] constexpr std::size_t edge_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    switch (edge) {
        case QscEdge::west:
            return core_index(0, parameter);
        case QscEdge::east:
            return core_index(format_v1::core_vertices - 1U, parameter);
        case QscEdge::south:
            return core_index(parameter, 0);
        case QscEdge::north:
            return core_index(parameter, format_v1::core_vertices - 1U);
    }
    return 0;
}

[[nodiscard]] constexpr std::size_t interior_edge_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    switch (edge) {
        case QscEdge::west:
            return core_index(1, parameter);
        case QscEdge::east:
            return core_index(format_v1::core_vertices - 2U, parameter);
        case QscEdge::south:
            return core_index(parameter, 1);
        case QscEdge::north:
            return core_index(parameter, format_v1::core_vertices - 2U);
    }
    return 0;
}

[[nodiscard]] constexpr std::size_t apron_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    constexpr std::size_t width = format_v1::serialized_elevation_samples;
    switch (edge) {
        case QscEdge::west:
            return (std::size_t{parameter} + 1U) * width;
        case QscEdge::east:
            return (std::size_t{parameter} + 1U) * width + width - 1U;
        case QscEdge::south:
            return std::size_t{parameter} + 1U;
        case QscEdge::north:
            return (width - 1U) * width + std::size_t{parameter} + 1U;
    }
    return 0;
}

[[nodiscard]] std::uint16_t quantize(const double elevation) {
    return static_cast<std::uint16_t>(std::nearbyint((elevation + 16'384.0) / 0.5));
}

[[nodiscard]] ElevationSampler face_discontinuous_sampler() {
    return [](const QscCoordinate coordinate) {
        return Result<double>::success(
            100.0 * static_cast<double>(coordinate.face) +
            8.0 * coordinate.u + 3.0 * coordinate.v);
    };
}

TEST_CASE("M4 resolves equatorial polar reversed edges and cube corners before quantization") {
    TemporaryDirectory temporary;
    const ElevationSampler sampler = face_discontinuous_sampler();
    const Sha256Digest dependency{};
    std::vector<StagedElevationTile> staged;
    staged.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto key = LunarTileKey::create(face, 0, 0, 0);
        REQUIRE(key);
        auto tile = stage_elevation_tile(
            key.value(), dependency, temporary.path() / ".ltbuild" / "staging", sampler);
        REQUIRE(tile);
        CHECK(std::filesystem::exists(tile.value().artifact_path));
        CHECK(tile.value().artifact_path.parent_path() ==
              temporary.path() / ".ltbuild" / "staging");
        CHECK(tile.value().artifact_path.filename().string().find(dependency.to_hex()) !=
              std::string::npos);
        CHECK(std::filesystem::file_size(tile.value().artifact_path) ==
              52U + std::uintmax_t{format_v1::core_vertices} *
                        format_v1::core_vertices * sizeof(double));
        auto loaded = load_staged_elevation_tile(
            tile.value().artifact_path, key.value(), dependency);
        REQUIRE(loaded);
        CHECK(loaded.value().core_samples == tile.value().core_samples);
        staged.push_back(std::move(tile).value());
    }

    auto reordered = staged;
    std::ranges::reverse(reordered);
    REQUIRE(resolve_elevation_boundaries(staged));
    REQUIRE(resolve_elevation_boundaries(reordered));
    std::ranges::sort(reordered, {}, [](const StagedElevationTile& tile) {
        return tile.key;
    });
    for (std::size_t index = 0; index < staged.size(); ++index) {
        CHECK(staged[index].key == reordered[index].key);
        CHECK(staged[index].core_samples == reordered[index].core_samples);
    }
    std::map<std::uint64_t, std::size_t> by_key;
    for (std::size_t index = 0; index < staged.size(); ++index) {
        by_key.emplace(staged[index].key.encoded(), index);
    }
    std::uint64_t unique_edges = 0;
    for (std::size_t index = 0; index < staged.size(); ++index) {
        for (const QscEdge edge : edges) {
            auto neighbor = qsc_tile_neighbor(staged[index].key, edge);
            REQUIRE(neighbor);
            if (staged[index].key >= neighbor.value().key) {
                continue;
            }
            const auto neighbor_index = by_key.at(neighbor.value().key.encoded());
            for (std::uint16_t parameter = 0;
                 parameter < format_v1::core_vertices;
                 ++parameter) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                CHECK(staged[index].core_samples[edge_index(edge, parameter)] ==
                      staged[neighbor_index].core_samples[edge_index(
                          neighbor.value().touching_edge, mapped)]);
            }
            constexpr std::uint16_t interior_parameter = 128;
            auto owner_u = QscProjection::LatticeCoordinate(
                staged[index].key.x(),
                edge == QscEdge::west ? std::uint16_t{0} :
                    (edge == QscEdge::east ? format_v1::core_vertices - 1U :
                                             interior_parameter),
                staged[index].key.level());
            auto owner_v = QscProjection::LatticeCoordinate(
                staged[index].key.y(),
                edge == QscEdge::south ? std::uint16_t{0} :
                    (edge == QscEdge::north ? format_v1::core_vertices - 1U :
                                              interior_parameter),
                staged[index].key.level());
            REQUIRE(owner_u);
            REQUIRE(owner_v);
            auto expected_owner = sampler(QscCoordinate{
                static_cast<QscFace>(staged[index].key.face()),
                owner_u.value(),
                owner_v.value(),
                0.0});
            REQUIRE(expected_owner);
            CHECK(staged[index].core_samples[edge_index(edge, interior_parameter)] ==
                  expected_owner.value());
            ++unique_edges;
        }
    }
    CHECK(unique_edges == 12);

    auto finalized = finalize_elevation_tiles(staged, sampler);
    REQUIRE(finalized);
    REQUIRE(finalized.value().size() == staged.size());
    for (std::size_t index = 0; index < finalized.value().size(); ++index) {
        const FinalizedElevationTile& tile = finalized.value()[index];
        CHECK(tile.quantized_core.size() ==
              std::size_t{format_v1::core_vertices} * format_v1::core_vertices);
        CHECK(tile.serialized_samples.size() ==
              std::size_t{format_v1::serialized_elevation_samples} *
                  format_v1::serialized_elevation_samples);
        for (const QscEdge edge : edges) {
            auto neighbor = qsc_tile_neighbor(tile.key, edge);
            REQUIRE(neighbor);
            const FinalizedElevationTile& adjacent =
                finalized.value()[by_key.at(neighbor.value().key.encoded())];
            for (std::uint16_t parameter = 0;
                 parameter < format_v1::core_vertices;
                 ++parameter) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                CHECK(tile.serialized_samples[apron_index(edge, parameter)] ==
                      adjacent.quantized_core[interior_edge_index(
                          neighbor.value().touching_edge, mapped)]);
            }
        }
    }
}

TEST_CASE("M4 ordinary and sparse aprons use quantized neighbor cores and virtual topology strips") {
    TemporaryDirectory temporary;
    const ElevationSampler sampler = face_discontinuous_sampler();
    const Sha256Digest dependency{};
    std::vector<LunarTileKey> keys;
    for (const auto [face, x, y] : std::array{
             std::array<std::uint32_t, 3>{0, 1, 1},
             std::array<std::uint32_t, 3>{0, 2, 1},
             std::array<std::uint32_t, 3>{1, 2, 0}}) {
        auto key = LunarTileKey::create(
            static_cast<std::uint8_t>(face), 2, x, y);
        REQUIRE(key);
        keys.push_back(key.value());
    }

    std::vector<StagedElevationTile> staged;
    for (const LunarTileKey key : keys) {
        auto tile = stage_elevation_tile(
            key, dependency, temporary.path() / ".ltbuild" / "staging", sampler);
        REQUIRE(tile);
        staged.push_back(std::move(tile).value());
    }
    REQUIRE(resolve_elevation_boundaries(staged));
    auto finalized = finalize_elevation_tiles(staged, sampler);
    REQUIRE(finalized);

    std::map<std::uint64_t, std::size_t> by_key;
    for (std::size_t index = 0; index < finalized.value().size(); ++index) {
        by_key.emplace(finalized.value()[index].key.encoded(), index);
    }

    const FinalizedElevationTile& west = finalized.value()[by_key.at(keys[0].encoded())];
    const FinalizedElevationTile& east = finalized.value()[by_key.at(keys[1].encoded())];
    for (std::uint16_t parameter = 0;
         parameter < format_v1::core_vertices;
         ++parameter) {
        CHECK(west.serialized_samples[apron_index(QscEdge::east, parameter)] ==
              east.quantized_core[interior_edge_index(QscEdge::west, parameter)]);
    }

    const FinalizedElevationTile& polar = finalized.value()[by_key.at(keys[2].encoded())];
    auto virtual_neighbor = qsc_tile_neighbor(polar.key, QscEdge::south);
    REQUIRE(virtual_neighbor);
    CHECK(virtual_neighbor.value().reversed);
    CHECK(virtual_neighbor.value().key.face() == 5);
    for (const std::uint16_t parameter :
         {std::uint16_t{0}, std::uint16_t{37}, std::uint16_t{128}, std::uint16_t{256}}) {
        const std::uint16_t mapped = static_cast<std::uint16_t>(
            format_v1::core_vertices - 1U - parameter);
        std::uint16_t x = mapped;
        std::uint16_t y = mapped;
        switch (virtual_neighbor.value().touching_edge) {
            case QscEdge::west: x = 1; break;
            case QscEdge::east: x = format_v1::core_vertices - 2U; break;
            case QscEdge::south: y = 1; break;
            case QscEdge::north: y = format_v1::core_vertices - 2U; break;
        }
        auto u = QscProjection::LatticeCoordinate(
            virtual_neighbor.value().key.x(), x, virtual_neighbor.value().key.level());
        auto v = QscProjection::LatticeCoordinate(
            virtual_neighbor.value().key.y(), y, virtual_neighbor.value().key.level());
        REQUIRE(u);
        REQUIRE(v);
        auto expected = sampler(QscCoordinate{
            static_cast<QscFace>(virtual_neighbor.value().key.face()),
            u.value(),
            v.value(),
            0.0});
        REQUIRE(expected);
        CHECK(polar.serialized_samples[apron_index(QscEdge::south, parameter)] ==
              quantize(expected.value()));
    }
}

}  // namespace
}  // namespace lunar::terrain::builder
