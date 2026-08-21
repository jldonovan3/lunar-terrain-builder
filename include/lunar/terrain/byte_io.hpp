#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

[[nodiscard]] Result<std::uint64_t> checked_add(
    std::uint64_t left,
    std::uint64_t right) noexcept;
[[nodiscard]] Result<std::uint64_t> checked_multiply(
    std::uint64_t left,
    std::uint64_t right) noexcept;

class LittleEndianReader {
public:
    explicit LittleEndianReader(
        std::span<const std::byte> bytes,
        std::uint64_t base_offset = 0) noexcept
        : bytes_(bytes), base_offset_(base_offset) {}

    [[nodiscard]] std::uint64_t size() const noexcept {
        return static_cast<std::uint64_t>(bytes_.size());
    }

    [[nodiscard]] Result<std::span<const std::byte>> read_bytes(
        std::uint64_t offset,
        std::uint64_t byte_count) const;
    [[nodiscard]] Result<std::uint8_t> read_u8(std::uint64_t offset) const;
    [[nodiscard]] Result<std::uint16_t> read_u16(std::uint64_t offset) const;
    [[nodiscard]] Result<std::uint32_t> read_u32(std::uint64_t offset) const;
    [[nodiscard]] Result<std::uint64_t> read_u64(std::uint64_t offset) const;
    [[nodiscard]] Result<float> read_f32(std::uint64_t offset) const;
    [[nodiscard]] Result<double> read_f64(std::uint64_t offset) const;

private:
    [[nodiscard]] Error truncated_error(std::uint64_t offset) const;

    std::span<const std::byte> bytes_;
    std::uint64_t base_offset_{};
};

}  // namespace lunar::terrain
