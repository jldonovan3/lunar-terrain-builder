#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain::builder {

struct PackingTile {
    LunarTileKey key;
    std::uint64_t payload_bytes{};
};

struct CanonicalPackRange {
    std::size_t first_tile{};
    std::size_t tile_count{};
};

// Partitions sorted tiles without mixing Face/Level groups. Eight-byte payload
// alignment and the 64-byte pack header count toward the rollover target. A
// single oversized tile is emitted alone.
[[nodiscard]] Result<std::vector<CanonicalPackRange>> plan_canonical_pack_ranges(
    std::span<const PackingTile> sorted_tiles,
    std::uint64_t target_pack_bytes);

}  // namespace lunar::terrain::builder
