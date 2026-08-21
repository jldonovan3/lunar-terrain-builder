#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

// Reconstructs row-major U16 samples from little-endian Delta2D residuals.
[[nodiscard]] Result<std::vector<std::byte>> reverse_delta2d_u16(
    std::span<const std::byte> residual_bytes,
    std::uint32_t width,
    std::uint32_t height);

// Decodes one dictionary-free Zstandard frame with a declared content size.
[[nodiscard]] Result<std::vector<std::byte>> decompress_zstandard(
    std::span<const std::byte> stored_bytes,
    std::uint64_t expected_logical_bytes);

}  // namespace lunar::terrain
