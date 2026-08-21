#include <lunar/terrain/byte_io.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain {

Result<std::uint64_t> checked_add(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    if (right > std::numeric_limits<std::uint64_t>::max() - left) {
        return Result<std::uint64_t>::failure(
            Error{ErrorCode::arithmetic_overflow, "unsigned addition overflow"});
    }
    return Result<std::uint64_t>::success(left + right);
}

Result<std::uint64_t> checked_multiply(
    const std::uint64_t left,
    const std::uint64_t right) noexcept {
    if (left != 0 && right > std::numeric_limits<std::uint64_t>::max() / left) {
        return Result<std::uint64_t>::failure(
            Error{ErrorCode::arithmetic_overflow, "unsigned multiplication overflow"});
    }
    return Result<std::uint64_t>::success(left * right);
}

Error LittleEndianReader::truncated_error(const std::uint64_t offset) const {
    auto absolute = checked_add(base_offset_, offset);
    Error error{ErrorCode::truncated_data, "byte range extends beyond its containing data"};
    error.with_offset(absolute ? absolute.value() : base_offset_);
    return error;
}

Result<std::span<const std::byte>> LittleEndianReader::read_bytes(
    const std::uint64_t offset,
    const std::uint64_t byte_count) const {
    auto end = checked_add(offset, byte_count);
    if (!end) {
        Error error = std::move(end).error();
        auto absolute = checked_add(base_offset_, offset);
        error.with_offset(absolute ? absolute.value() : base_offset_);
        return Result<std::span<const std::byte>>::failure(std::move(error));
    }
    if (end.value() > size() || offset > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) ||
        byte_count > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
        return Result<std::span<const std::byte>>::failure(truncated_error(offset));
    }
    return Result<std::span<const std::byte>>::success(
        bytes_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(byte_count)));
}

Result<std::uint8_t> LittleEndianReader::read_u8(const std::uint64_t offset) const {
    auto data = read_bytes(offset, 1);
    if (!data) {
        return Result<std::uint8_t>::failure(std::move(data).error());
    }
    return Result<std::uint8_t>::success(std::to_integer<std::uint8_t>(data.value()[0]));
}

Result<std::uint16_t> LittleEndianReader::read_u16(const std::uint64_t offset) const {
    auto data = read_bytes(offset, 2);
    if (!data) {
        return Result<std::uint16_t>::failure(std::move(data).error());
    }
    const auto bytes = data.value();
    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[0]) |
        (std::to_integer<std::uint16_t>(bytes[1]) << 8U)));
}

Result<std::uint32_t> LittleEndianReader::read_u32(const std::uint64_t offset) const {
    auto data = read_bytes(offset, 4);
    if (!data) {
        return Result<std::uint32_t>::failure(std::move(data).error());
    }
    const auto bytes = data.value();
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[index]) << (index * 8U);
    }
    return Result<std::uint32_t>::success(value);
}

Result<std::uint64_t> LittleEndianReader::read_u64(const std::uint64_t offset) const {
    auto data = read_bytes(offset, 8);
    if (!data) {
        return Result<std::uint64_t>::failure(std::move(data).error());
    }
    const auto bytes = data.value();
    std::uint64_t value = 0;
    for (std::uint64_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[static_cast<std::size_t>(index)]) <<
                 (index * 8U);
    }
    return Result<std::uint64_t>::success(value);
}

Result<float> LittleEndianReader::read_f32(const std::uint64_t offset) const {
    auto bits = read_u32(offset);
    if (!bits) {
        return Result<float>::failure(std::move(bits).error());
    }
    return Result<float>::success(std::bit_cast<float>(bits.value()));
}

Result<double> LittleEndianReader::read_f64(const std::uint64_t offset) const {
    auto bits = read_u64(offset);
    if (!bits) {
        return Result<double>::failure(std::move(bits).error());
    }
    return Result<double>::success(std::bit_cast<double>(bits.value()));
}

}  // namespace lunar::terrain
