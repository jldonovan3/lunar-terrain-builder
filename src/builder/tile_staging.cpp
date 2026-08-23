#include "builder/tile_staging.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_topology.hpp>

namespace lunar::terrain::builder {
namespace {

using Bytes = std::vector<std::byte>;

constexpr std::size_t core_sample_count =
    std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
constexpr std::size_t serialized_sample_count =
    std::size_t{format_v1::serialized_elevation_samples} *
    format_v1::serialized_elevation_samples;
constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};

[[nodiscard]] Error staging_error(
    const ErrorCode code,
    std::string message,
    const std::optional<std::filesystem::path>& path = std::nullopt,
    const std::optional<LunarTileKey> key = std::nullopt) {
    Error error{code, std::move(message)};
    if (path) {
        error.with_path(path->string());
    }
    if (key) {
        error.with_tile_key(key->encoded());
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::optional<std::filesystem::path>& path = std::nullopt,
    const std::optional<LunarTileKey> key = std::nullopt) {
    return Result<T>::failure(
        staging_error(code, std::move(message), path, key));
}

void append_u16(Bytes& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value));
    bytes.push_back(static_cast<std::byte>(value >> 8U));
}

void append_u32(Bytes& bytes, const std::uint32_t value) {
    for (std::uint32_t byte = 0; byte < 4U; ++byte) {
        bytes.push_back(static_cast<std::byte>(value >> (byte * 8U)));
    }
}

void append_u64(Bytes& bytes, const std::uint64_t value) {
    for (std::uint32_t byte = 0; byte < 8U; ++byte) {
        bytes.push_back(static_cast<std::byte>(value >> (byte * 8U)));
    }
}

void append_bytes(Bytes& bytes, const std::span<const std::byte> value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

[[nodiscard]] constexpr std::size_t core_index(
    const std::uint16_t x,
    const std::uint16_t y) noexcept {
    return std::size_t{y} * format_v1::core_vertices + x;
}

[[nodiscard]] constexpr std::size_t stored_index(
    const std::uint16_t x,
    const std::uint16_t y) noexcept {
    return std::size_t{y} * format_v1::serialized_elevation_samples + x;
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

enum class Corner : std::uint8_t {
    south_west = 0,
    south_east = 1,
    north_west = 2,
    north_east = 3,
};

[[nodiscard]] constexpr Corner edge_corner(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    const bool high = parameter == format_v1::core_vertices - 1U;
    switch (edge) {
        case QscEdge::west:
            return high ? Corner::north_west : Corner::south_west;
        case QscEdge::east:
            return high ? Corner::north_east : Corner::south_east;
        case QscEdge::south:
            return high ? Corner::south_east : Corner::south_west;
        case QscEdge::north:
            return high ? Corner::north_east : Corner::north_west;
    }
    return Corner::south_west;
}

[[nodiscard]] constexpr std::size_t corner_core_index(const Corner corner) noexcept {
    switch (corner) {
        case Corner::south_west:
            return core_index(0, 0);
        case Corner::south_east:
            return core_index(format_v1::core_vertices - 1U, 0);
        case Corner::north_west:
            return core_index(0, format_v1::core_vertices - 1U);
        case Corner::north_east:
            return core_index(
                format_v1::core_vertices - 1U,
                format_v1::core_vertices - 1U);
    }
    return 0;
}

[[nodiscard]] constexpr std::size_t corner_node(
    const std::size_t tile_index,
    const Corner corner) noexcept {
    return tile_index * 4U + static_cast<std::size_t>(corner);
}

[[nodiscard]] std::size_t find_root(
    std::vector<std::size_t>& parents,
    const std::size_t node) noexcept {
    std::size_t root = node;
    while (parents[root] != root) {
        root = parents[root];
    }
    std::size_t current = node;
    while (parents[current] != current) {
        const std::size_t next = parents[current];
        parents[current] = root;
        current = next;
    }
    return root;
}

void unite(
    std::vector<std::size_t>& parents,
    const std::size_t first,
    const std::size_t second) noexcept {
    const std::size_t first_root = find_root(parents, first);
    const std::size_t second_root = find_root(parents, second);
    if (first_root != second_root) {
        parents[std::max(first_root, second_root)] = std::min(first_root, second_root);
    }
}

struct SamplePatch {
    std::size_t receiving_tile{};
    std::size_t receiving_sample{};
    std::size_t owning_tile{};
    std::size_t owning_sample{};
};

[[nodiscard]] Result<std::uint16_t> quantize_elevation(const double elevation) {
    if (!std::isfinite(elevation)) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument,
            "terrain sampler produced a non-finite elevation");
    }
    const double scaled = (elevation - (-16'384.0)) / 0.5;
    if (scaled < 0.0 || scaled > 65'535.0) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument,
            "terrain elevation is outside the v1 U16 profile");
    }
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    std::uint32_t rounded = static_cast<std::uint32_t>(lower);
    if (fraction > 0.5 || (fraction == 0.5 && (rounded & 1U) != 0)) {
        ++rounded;
    }
    if (rounded > std::numeric_limits<std::uint16_t>::max()) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument,
            "rounded terrain elevation is outside U16");
    }
    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(rounded));
}

[[nodiscard]] std::filesystem::path artifact_path(
    const std::filesystem::path& staging_directory,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash) {
    return staging_directory / fmt::format(
        "{:016x}-{}.core-v1", key.encoded(), dependency_hash.to_hex());
}

[[nodiscard]] Bytes serialize_staged_core(
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::span<const double> samples) {
    Bytes bytes;
    bytes.reserve(52U + samples.size() * sizeof(double));
    for (const char character : std::string_view{"LTSC"}) {
        bytes.push_back(static_cast<std::byte>(character));
    }
    append_u16(bytes, 1);
    append_u16(bytes, 0);
    append_u64(bytes, key.encoded());
    append_bytes(bytes, dependency_hash.bytes);
    append_u32(bytes, static_cast<std::uint32_t>(samples.size()));
    for (const double sample : samples) {
        append_u64(bytes, std::bit_cast<std::uint64_t>(sample));
    }
    return bytes;
}

[[nodiscard]] Result<Bytes> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return failure<Bytes>(ErrorCode::io_error, "could not open staging artifact", path);
    }
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) >
                        std::numeric_limits<std::size_t>::max()) {
        return failure<Bytes>(ErrorCode::arithmetic_overflow, "staging artifact is too large", path);
    }
    Bytes bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!stream) {
        return failure<Bytes>(ErrorCode::io_error, "could not read staging artifact", path);
    }
    return Result<Bytes>::success(std::move(bytes));
}

[[nodiscard]] Result<void> persist_atomically(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes,
    const LunarTileKey key) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(staging_error(
            ErrorCode::io_error,
            fmt::format("could not create staging directory: {}", filesystem_error.message()),
            path.parent_path(),
            key));
    }

    if (std::filesystem::exists(path, filesystem_error)) {
        if (filesystem_error) {
            return Result<void>::failure(staging_error(
                ErrorCode::io_error,
                fmt::format("could not inspect staging artifact: {}", filesystem_error.message()),
                path,
                key));
        }
        auto existing = read_file(path);
        if (!existing) {
            return Result<void>::failure(std::move(existing).error());
        }
        if (std::ranges::equal(existing.value(), bytes)) {
            return Result<void>::success();
        }
        return Result<void>::failure(staging_error(
            ErrorCode::hash_mismatch,
            "dependency-identical staged core has different bytes",
            path,
            key));
    }

    const std::filesystem::path temporary =
        path.parent_path() / ("." + path.filename().string() + ".tmp");
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream.is_open()) {
            return Result<void>::failure(staging_error(
                ErrorCode::io_error, "could not create temporary staging artifact", temporary, key));
        }
        stream.write(
            reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
        stream.flush();
        if (!stream) {
            stream.close();
            std::filesystem::remove(temporary, filesystem_error);
            return Result<void>::failure(staging_error(
                ErrorCode::io_error, "could not write temporary staging artifact", temporary, key));
        }
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        return Result<void>::failure(staging_error(
            ErrorCode::io_error,
            fmt::format("could not atomically publish staging artifact: {}", filesystem_error.message()),
            path,
            key));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<double> sample_core_coordinate(
    const LunarTileKey key,
    const std::uint16_t x,
    const std::uint16_t y,
    const ElevationSampler& sampler) {
    auto u = QscProjection::LatticeCoordinate(key.x(), x, key.level());
    auto v = QscProjection::LatticeCoordinate(key.y(), y, key.level());
    if (!u || !v) {
        return Result<double>::failure(u ? std::move(v).error() : std::move(u).error());
    }
    auto sampled = sampler(QscCoordinate{
        static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
    if (!sampled) {
        Error error = std::move(sampled).error();
        error.with_tile_key(key.encoded());
        return Result<double>::failure(std::move(error));
    }
    if (!std::isfinite(sampled.value())) {
        return failure<double>(
            ErrorCode::invalid_argument,
            "terrain sampler produced a non-finite elevation",
            std::nullopt,
            key);
    }
    return sampled;
}

[[nodiscard]] Result<std::uint16_t> sample_virtual_apron(
    const LunarTileKey key,
    const QscEdge edge,
    const std::uint16_t parameter,
    const ElevationSampler& sampler) {
    auto neighbor = qsc_tile_neighbor(key, edge);
    if (!neighbor) {
        return Result<std::uint16_t>::failure(std::move(neighbor).error());
    }
    const std::uint16_t mapped = neighbor.value().reversed
        ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
        : parameter;
    std::uint16_t x = mapped;
    std::uint16_t y = mapped;
    switch (neighbor.value().touching_edge) {
        case QscEdge::west:
            x = 1;
            break;
        case QscEdge::east:
            x = format_v1::core_vertices - 2U;
            break;
        case QscEdge::south:
            y = 1;
            break;
        case QscEdge::north:
            y = format_v1::core_vertices - 2U;
            break;
    }
    auto sampled = sample_core_coordinate(neighbor.value().key, x, y, sampler);
    if (!sampled) {
        return Result<std::uint16_t>::failure(std::move(sampled).error());
    }
    auto quantized = quantize_elevation(sampled.value());
    if (!quantized) {
        Error error = std::move(quantized).error();
        error.with_tile_key(key.encoded());
        return Result<std::uint16_t>::failure(std::move(error));
    }
    return quantized;
}

[[nodiscard]] Result<QscCoordinate> map_extended_coordinate(QscCoordinate coordinate) {
    for (std::uint8_t transition = 0; transition < 4U; ++transition) {
        std::optional<QscEdge> crossed;
        double depth = 0.0;
        if (coordinate.u < -1.0) {
            crossed = QscEdge::west;
            depth = -1.0 - coordinate.u;
        } else if (coordinate.u > 1.0) {
            crossed = QscEdge::east;
            depth = coordinate.u - 1.0;
        } else if (coordinate.v < -1.0) {
            crossed = QscEdge::south;
            depth = -1.0 - coordinate.v;
        } else if (coordinate.v > 1.0) {
            crossed = QscEdge::north;
            depth = coordinate.v - 1.0;
        } else {
            return Result<QscCoordinate>::success(coordinate);
        }

        const bool vertical = *crossed == QscEdge::west || *crossed == QscEdge::east;
        double parameter = vertical ? coordinate.v : coordinate.u;
        const QscEdgeConnection connection = qsc_edge_connection(coordinate.face, *crossed);
        if (connection.reversed) {
            parameter = -parameter;
        }
        coordinate.face = connection.face;
        switch (connection.edge) {
            case QscEdge::west:
                coordinate.u = -1.0 + depth;
                coordinate.v = parameter;
                break;
            case QscEdge::east:
                coordinate.u = 1.0 - depth;
                coordinate.v = parameter;
                break;
            case QscEdge::south:
                coordinate.u = parameter;
                coordinate.v = -1.0 + depth;
                break;
            case QscEdge::north:
                coordinate.u = parameter;
                coordinate.v = 1.0 - depth;
                break;
        }
    }
    return failure<QscCoordinate>(
        ErrorCode::internal_error,
        "could not map an apron corner through QSC topology");
}

[[nodiscard]] Result<std::uint16_t> sample_apron_corner(
    const LunarTileKey key,
    const bool east,
    const bool north,
    const ElevationSampler& sampler) {
    const std::int64_t denominator = static_cast<std::int64_t>(
        std::uint64_t{format_v1::tile_cells} << key.level());
    const auto extended_coordinate = [denominator](
        const std::uint32_t tile_coordinate,
        const bool high) {
        const std::int64_t grid = static_cast<std::int64_t>(
            std::uint64_t{tile_coordinate} * format_v1::tile_cells) +
            (high ? static_cast<std::int64_t>(format_v1::core_vertices)
                  : std::int64_t{-1});
        return static_cast<double>(2 * grid - denominator) /
               static_cast<double>(denominator);
    };
    auto mapped = map_extended_coordinate(QscCoordinate{
        static_cast<QscFace>(key.face()),
        extended_coordinate(key.x(), east),
        extended_coordinate(key.y(), north),
        0.0,
    });
    if (!mapped) {
        return Result<std::uint16_t>::failure(std::move(mapped).error());
    }
    auto sampled = sampler(mapped.value());
    if (!sampled) {
        Error error = std::move(sampled).error();
        error.with_tile_key(key.encoded());
        return Result<std::uint16_t>::failure(std::move(error));
    }
    auto quantized = quantize_elevation(sampled.value());
    if (!quantized) {
        Error error = std::move(quantized).error();
        error.with_tile_key(key.encoded());
        return Result<std::uint16_t>::failure(std::move(error));
    }
    return quantized;
}

void write_apron_sample(
    std::vector<std::uint16_t>& samples,
    const QscEdge edge,
    const std::uint16_t parameter,
    const std::uint16_t value) {
    switch (edge) {
        case QscEdge::west:
            samples[stored_index(0, static_cast<std::uint16_t>(parameter + 1U))] = value;
            break;
        case QscEdge::east:
            samples[stored_index(
                format_v1::serialized_elevation_samples - 1U,
                static_cast<std::uint16_t>(parameter + 1U))] = value;
            break;
        case QscEdge::south:
            samples[stored_index(static_cast<std::uint16_t>(parameter + 1U), 0)] = value;
            break;
        case QscEdge::north:
            samples[stored_index(
                static_cast<std::uint16_t>(parameter + 1U),
                format_v1::serialized_elevation_samples - 1U)] = value;
            break;
    }
}

}  // namespace

Result<StagedElevationTile> stage_elevation_tile(
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::filesystem::path& staging_directory,
    const ElevationSampler& sampler) {
    if (!sampler) {
        return failure<StagedElevationTile>(
            ErrorCode::invalid_argument, "elevation sampler is empty", std::nullopt, key);
    }
    std::vector<double> samples(core_sample_count);
    for (std::uint16_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::uint16_t x = 0; x < format_v1::core_vertices; ++x) {
            auto sampled = sample_core_coordinate(key, x, y, sampler);
            if (!sampled) {
                return Result<StagedElevationTile>::failure(std::move(sampled).error());
            }
            samples[core_index(x, y)] = sampled.value();
        }
    }

    const std::filesystem::path path = artifact_path(staging_directory, key, dependency_hash);
    const Bytes bytes = serialize_staged_core(key, dependency_hash, samples);
    auto persisted = persist_atomically(path, bytes, key);
    if (!persisted) {
        return Result<StagedElevationTile>::failure(std::move(persisted).error());
    }
    return Result<StagedElevationTile>::success(
        StagedElevationTile{key, dependency_hash, std::move(samples), path});
}

Result<void> resolve_elevation_boundaries(const std::span<StagedElevationTile> tiles) {
    if (tiles.empty()) {
        return Result<void>::success();
    }

    std::map<std::uint64_t, std::size_t> by_key;
    std::optional<std::uint8_t> level;
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        if (tiles[index].core_samples.size() != core_sample_count) {
            return Result<void>::failure(staging_error(
                ErrorCode::invalid_argument,
                "staged tile core does not contain 257x257 samples",
                tiles[index].artifact_path,
                tiles[index].key));
        }
        if (level && *level != tiles[index].key.level()) {
            return Result<void>::failure(staging_error(
                ErrorCode::invalid_argument,
                "one seam-resolution batch must contain a single QSC level",
                std::nullopt,
                tiles[index].key));
        }
        level = tiles[index].key.level();
        if (!by_key.emplace(tiles[index].key.encoded(), index).second) {
            return Result<void>::failure(staging_error(
                ErrorCode::invalid_argument,
                "seam-resolution batch contains a duplicate TileKey",
                std::nullopt,
                tiles[index].key));
        }
    }

    std::vector<std::size_t> corner_parents(tiles.size() * 4U);
    std::iota(corner_parents.begin(), corner_parents.end(), std::size_t{0});
    std::vector<SamplePatch> patches;
    patches.reserve(tiles.size() * 4U * format_v1::core_vertices);

    for (std::size_t tile_index = 0; tile_index < tiles.size(); ++tile_index) {
        const StagedElevationTile& tile = tiles[tile_index];
        for (const QscEdge edge : edges) {
            auto neighbor = qsc_tile_neighbor(tile.key, edge);
            if (!neighbor) {
                return Result<void>::failure(std::move(neighbor).error());
            }
            const auto found = by_key.find(neighbor.value().key.encoded());
            if (found == by_key.end() || tile.key >= neighbor.value().key) {
                continue;
            }
            const std::size_t neighbor_index = found->second;
            for (const std::uint16_t endpoint :
                 {std::uint16_t{0}, std::uint16_t{format_v1::core_vertices - 1U}}) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - endpoint)
                    : endpoint;
                unite(
                    corner_parents,
                    corner_node(tile_index, edge_corner(edge, endpoint)),
                    corner_node(
                        neighbor_index,
                        edge_corner(neighbor.value().touching_edge, mapped)));
            }
            for (std::uint16_t parameter = 1;
                 parameter + 1U < format_v1::core_vertices;
                 ++parameter) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                patches.push_back(SamplePatch{
                    neighbor_index,
                    edge_index(neighbor.value().touching_edge, mapped),
                    tile_index,
                    edge_index(edge, parameter),
                });
            }
        }
    }

    std::map<std::size_t, std::vector<std::size_t>> corner_groups;
    for (std::size_t node = 0; node < corner_parents.size(); ++node) {
        corner_groups[find_root(corner_parents, node)].push_back(node);
    }
    for (const auto& [unused_root, nodes] : corner_groups) {
        static_cast<void>(unused_root);
        if (nodes.size() < 2U) {
            continue;
        }
        const auto owner = *std::ranges::min_element(nodes, [&tiles](
            const std::size_t left,
            const std::size_t right) {
            const std::size_t left_tile = left / 4U;
            const std::size_t right_tile = right / 4U;
            return std::pair{tiles[left_tile].key.encoded(), left % 4U} <
                   std::pair{tiles[right_tile].key.encoded(), right % 4U};
        });
        for (const std::size_t node : nodes) {
            if (node == owner) {
                continue;
            }
            patches.push_back(SamplePatch{
                node / 4U,
                corner_core_index(static_cast<Corner>(node % 4U)),
                owner / 4U,
                corner_core_index(static_cast<Corner>(owner % 4U)),
            });
        }
    }

    std::ranges::sort(patches, [&tiles](const SamplePatch& left, const SamplePatch& right) {
        return std::array{
                   tiles[left.receiving_tile].key.encoded(),
                   static_cast<std::uint64_t>(left.receiving_sample),
                   tiles[left.owning_tile].key.encoded(),
                   static_cast<std::uint64_t>(left.owning_sample)} <
               std::array{
                   tiles[right.receiving_tile].key.encoded(),
                   static_cast<std::uint64_t>(right.receiving_sample),
                   tiles[right.owning_tile].key.encoded(),
                   static_cast<std::uint64_t>(right.owning_sample)};
    });
    std::size_t patch_index = 0;
    while (patch_index < patches.size()) {
        const std::size_t receiving_tile = patches[patch_index].receiving_tile;
        std::size_t group_end = patch_index;
        while (group_end < patches.size() &&
               patches[group_end].receiving_tile == receiving_tile) {
            ++group_end;
        }
        for (std::size_t index = patch_index; index < group_end; ++index) {
            const SamplePatch& patch = patches[index];
            tiles[receiving_tile].core_samples[patch.receiving_sample] =
                tiles[patch.owning_tile].core_samples[patch.owning_sample];
        }
        patch_index = group_end;
    }
    return Result<void>::success();
}

Result<std::vector<FinalizedElevationTile>> finalize_elevation_tiles(
    const std::span<const StagedElevationTile> tiles,
    const ElevationSampler& sampler) {
    if (!sampler) {
        return failure<std::vector<FinalizedElevationTile>>(
            ErrorCode::invalid_argument, "elevation sampler is empty");
    }

    std::vector<FinalizedElevationTile> finalized;
    finalized.reserve(tiles.size());
    std::map<std::uint64_t, std::size_t> by_key;
    for (const StagedElevationTile& staged : tiles) {
        if (staged.core_samples.size() != core_sample_count) {
            return failure<std::vector<FinalizedElevationTile>>(
                ErrorCode::invalid_argument,
                "staged tile core does not contain 257x257 samples",
                staged.artifact_path,
                staged.key);
        }
        FinalizedElevationTile tile{
            staged.key,
            staged.dependency_hash,
            {},
            {},
        };
        tile.quantized_core.reserve(core_sample_count);
        for (const double sample : staged.core_samples) {
            auto quantized = quantize_elevation(sample);
            if (!quantized) {
                Error error = std::move(quantized).error();
                error.with_tile_key(staged.key.encoded());
                return Result<std::vector<FinalizedElevationTile>>::failure(std::move(error));
            }
            tile.quantized_core.push_back(quantized.value());
        }
        tile.serialized_samples.assign(serialized_sample_count, 0);
        for (std::uint16_t y = 0; y < format_v1::core_vertices; ++y) {
            for (std::uint16_t x = 0; x < format_v1::core_vertices; ++x) {
                tile.serialized_samples[stored_index(
                    static_cast<std::uint16_t>(x + 1U),
                    static_cast<std::uint16_t>(y + 1U))] =
                    tile.quantized_core[core_index(x, y)];
            }
        }
        by_key.emplace(tile.key.encoded(), finalized.size());
        finalized.push_back(std::move(tile));
    }

    for (FinalizedElevationTile& tile : finalized) {
        for (const QscEdge edge : edges) {
            auto neighbor = qsc_tile_neighbor(tile.key, edge);
            if (!neighbor) {
                return Result<std::vector<FinalizedElevationTile>>::failure(
                    std::move(neighbor).error());
            }
            const auto found = by_key.find(neighbor.value().key.encoded());
            for (std::uint16_t parameter = 0;
                 parameter < format_v1::core_vertices;
                 ++parameter) {
                const std::uint16_t mapped = neighbor.value().reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                std::uint16_t value{};
                if (found != by_key.end()) {
                    value = finalized[found->second].quantized_core[interior_edge_index(
                        neighbor.value().touching_edge, mapped)];
                } else {
                    auto sampled = sample_virtual_apron(tile.key, edge, parameter, sampler);
                    if (!sampled) {
                        return Result<std::vector<FinalizedElevationTile>>::failure(
                            std::move(sampled).error());
                    }
                    value = sampled.value();
                }
                write_apron_sample(tile.serialized_samples, edge, parameter, value);
            }
        }

        for (const auto [east, north, x, y] : std::array{
                 std::array<std::uint16_t, 4>{0, 0, 0, 0},
                 std::array<std::uint16_t, 4>{1, 0, format_v1::serialized_elevation_samples - 1U, 0},
                 std::array<std::uint16_t, 4>{0, 1, 0, format_v1::serialized_elevation_samples - 1U},
                 std::array<std::uint16_t, 4>{1, 1, format_v1::serialized_elevation_samples - 1U,
                                             format_v1::serialized_elevation_samples - 1U}}) {
            auto sampled = sample_apron_corner(tile.key, east != 0, north != 0, sampler);
            if (!sampled) {
                return Result<std::vector<FinalizedElevationTile>>::failure(
                    std::move(sampled).error());
            }
            tile.serialized_samples[stored_index(x, y)] = sampled.value();
        }
    }
    return Result<std::vector<FinalizedElevationTile>>::success(std::move(finalized));
}

}  // namespace lunar::terrain::builder
