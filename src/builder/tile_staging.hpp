#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain::builder {

using ElevationSampler = std::function<Result<double>(QscCoordinate)>;

struct StagedElevationTile {
    LunarTileKey key;
    Sha256Digest dependency_hash;
    std::vector<double> core_samples;
    std::filesystem::path artifact_path;
};

struct FinalizedElevationTile {
    LunarTileKey key;
    Sha256Digest dependency_hash;
    std::vector<std::uint16_t> quantized_core;
    std::vector<std::uint16_t> serialized_samples;
};

[[nodiscard]] std::filesystem::path staged_elevation_artifact_path(
    const std::filesystem::path& staging_directory,
    LunarTileKey key,
    const Sha256Digest& dependency_hash);

// Loads and validates every field of a dependency-named staged core. Cache
// metadata alone never makes an artifact reusable.
[[nodiscard]] Result<StagedElevationTile> load_staged_elevation_tile(
    const std::filesystem::path& artifact_path,
    LunarTileKey expected_key,
    const Sha256Digest& expected_dependency_hash);

// Samples and atomically persists one independent 257x257 binary64 core.
// The artifact name includes the complete dependency hash and its contents use
// explicit little-endian fields; staging artifacts are disposable build state.
[[nodiscard]] Result<StagedElevationTile> stage_elevation_tile(
    LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::filesystem::path& staging_directory,
    const ElevationSampler& sampler);

[[nodiscard]] Result<StagedElevationTile> stage_elevation_tile_samples(
    LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::filesystem::path& staging_directory,
    std::span<const double> samples);

// Builds same-level adjacency from the supplied keys, collects deterministic
// edge and corner patches, sorts them, and applies them once per receiver. The
// lowest encoded TileKey owns every shared boundary value.
[[nodiscard]] Result<void> resolve_elevation_boundaries(
    std::span<StagedElevationTile> tiles);

// Quantizes seam-resolved cores and then builds their 259x259 channels.
// Materialized neighbors supply ordinary aprons. Missing neighbors are sampled
// as virtual strips through the explicit QSC topology mapping.
[[nodiscard]] Result<std::vector<FinalizedElevationTile>> finalize_elevation_tiles(
    std::span<const StagedElevationTile> tiles,
    const ElevationSampler& sampler);

}  // namespace lunar::terrain::builder
