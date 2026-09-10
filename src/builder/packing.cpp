#include "builder/packing.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>

namespace lunar::terrain::builder {
namespace {

[[nodiscard]] constexpr std::uint64_t align8(const std::uint64_t value) noexcept {
    return (value + 7U) & ~std::uint64_t{7};
}

}  // namespace

Result<std::vector<CanonicalPackRange>> plan_canonical_pack_ranges(
    const std::span<const PackingTile> sorted_tiles,
    const std::uint64_t target_pack_bytes,
    const std::stop_token cancellation) {
    if (sorted_tiles.empty() || target_pack_bytes == 0) {
        return Result<std::vector<CanonicalPackRange>>::failure(Error{
            ErrorCode::invalid_argument,
            "canonical pack planning requires nonempty tiles and a positive target size"});
    }
    for (std::size_t index = 0; index < sorted_tiles.size(); ++index) {
        if ((index % 256U) == 0U && cancellation.stop_requested()) {
            return Result<std::vector<CanonicalPackRange>>::failure(
                Error{ErrorCode::cancelled, "canonical pack planning was cancelled"});
        }
        if (sorted_tiles[index].payload_bytes > std::numeric_limits<std::uint32_t>::max()) {
            return Result<std::vector<CanonicalPackRange>>::failure(Error{
                ErrorCode::arithmetic_overflow, "tile payload exceeds the v1 uint32 size limit"}
                .with_tile_key(sorted_tiles[index].key.encoded()));
        }
        if (index != 0 && sorted_tiles[index].key <= sorted_tiles[index - 1U].key) {
            return Result<std::vector<CanonicalPackRange>>::failure(Error{
                ErrorCode::invalid_argument,
                "canonical pack planning requires strictly increasing TileKeys"}
                .with_tile_key(sorted_tiles[index].key.encoded()));
        }
    }

    std::vector<CanonicalPackRange> ranges;
    std::size_t first = 0;
    while (first < sorted_tiles.size()) {
        if (cancellation.stop_requested()) {
            return Result<std::vector<CanonicalPackRange>>::failure(
                Error{ErrorCode::cancelled, "canonical pack planning was cancelled"});
        }
        const std::uint8_t face = sorted_tiles[first].key.face();
        const std::uint8_t level = sorted_tiles[first].key.level();
        std::uint64_t bytes = format_v1::bytes::pack_header;
        std::size_t end = first;
        while (end < sorted_tiles.size() &&
               sorted_tiles[end].key.face() == face &&
               sorted_tiles[end].key.level() == level) {
            if ((end % 256U) == 0U && cancellation.stop_requested()) {
                return Result<std::vector<CanonicalPackRange>>::failure(
                    Error{ErrorCode::cancelled, "canonical pack planning was cancelled"});
            }
            const std::uint64_t aligned = align8(bytes);
            if (sorted_tiles[end].payload_bytes >
                std::numeric_limits<std::uint64_t>::max() - aligned) {
                return Result<std::vector<CanonicalPackRange>>::failure(Error{
                    ErrorCode::arithmetic_overflow, "canonical pack size overflows uint64"}
                    .with_tile_key(sorted_tiles[end].key.encoded()));
            }
            const std::uint64_t candidate = aligned + sorted_tiles[end].payload_bytes;
            if (end != first && candidate > target_pack_bytes) {
                break;
            }
            bytes = candidate;
            ++end;
        }
        ranges.push_back(CanonicalPackRange{first, end - first});
        first = end;
    }
    return Result<std::vector<CanonicalPackRange>>::success(std::move(ranges));
}

}  // namespace lunar::terrain::builder
