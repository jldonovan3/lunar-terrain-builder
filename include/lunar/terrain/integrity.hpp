#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

[[nodiscard]] std::uint32_t crc32c(std::span<const std::byte> bytes) noexcept;
[[nodiscard]] Result<Sha256Digest> sha256(std::span<const std::byte> bytes);

}  // namespace lunar::terrain
