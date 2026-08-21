#include <lunar/terrain/codec.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <zstd.h>

#include <lunar/terrain/byte_io.hpp>
#include <lunar/terrain/error.hpp>

namespace lunar::terrain {

Result<std::vector<std::byte>> reverse_delta2d_u16(
    const std::span<const std::byte> residual_bytes,
    const std::uint32_t width,
    const std::uint32_t height) {
    if (width == 0 || height == 0) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::invalid_argument, "Delta2D dimensions must be nonzero"});
    }

    auto sample_count = checked_multiply(width, height);
    if (!sample_count) {
        return Result<std::vector<std::byte>>::failure(std::move(sample_count).error());
    }
    auto required_bytes = checked_multiply(sample_count.value(), 2);
    if (!required_bytes) {
        return Result<std::vector<std::byte>>::failure(std::move(required_bytes).error());
    }
    if (required_bytes.value() != residual_bytes.size() ||
        required_bytes.value() > std::numeric_limits<std::size_t>::max()) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::invalid_format, "Delta2D byte count does not match its dimensions"});
    }

    std::vector<std::uint16_t> samples(static_cast<std::size_t>(sample_count.value()));
    LittleEndianReader reader{residual_bytes};
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            const std::uint64_t sample_index = std::uint64_t{y} * width + x;
            auto residual = reader.read_u16(sample_index * 2U);
            if (!residual) {
                return Result<std::vector<std::byte>>::failure(std::move(residual).error());
            }

            std::uint16_t predicted = 0;
            if (y == 0 && x > 0) {
                predicted = samples[static_cast<std::size_t>(sample_index - 1U)];
            } else if (x == 0 && y > 0) {
                predicted = samples[static_cast<std::size_t>(sample_index - width)];
            } else if (x > 0 && y > 0) {
                const std::uint32_t value =
                    std::uint32_t{samples[static_cast<std::size_t>(sample_index - 1U)]} +
                    std::uint32_t{samples[static_cast<std::size_t>(sample_index - width)]} -
                    std::uint32_t{samples[static_cast<std::size_t>(sample_index - width - 1U)]};
                predicted = static_cast<std::uint16_t>(value);
            }
            samples[static_cast<std::size_t>(sample_index)] =
                static_cast<std::uint16_t>(std::uint32_t{predicted} + residual.value());
        }
    }

    std::vector<std::byte> decoded(static_cast<std::size_t>(required_bytes.value()));
    for (std::size_t index = 0; index < samples.size(); ++index) {
        decoded[index * 2U] = static_cast<std::byte>(samples[index] & 0xFFU);
        decoded[index * 2U + 1U] = static_cast<std::byte>(samples[index] >> 8U);
    }
    return Result<std::vector<std::byte>>::success(std::move(decoded));
}

Result<std::vector<std::byte>> decompress_zstandard(
    const std::span<const std::byte> stored_bytes,
    const std::uint64_t expected_logical_bytes) {
    if (stored_bytes.empty()) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::decompression_failed, "Zstandard frame is empty"});
    }
    if (expected_logical_bytes > std::numeric_limits<std::size_t>::max()) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::arithmetic_overflow, "Zstandard output does not fit in memory"});
    }
    if (ZSTD_getDictID_fromFrame(stored_bytes.data(), stored_bytes.size()) != 0) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::unsupported_feature, "Zstandard dictionaries are not supported by format v1"});
    }

    const unsigned long long frame_size =
        ZSTD_getFrameContentSize(stored_bytes.data(), stored_bytes.size());
    if (frame_size == ZSTD_CONTENTSIZE_ERROR) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::decompression_failed, "stored bytes are not a valid Zstandard frame"});
    }
    if (frame_size == ZSTD_CONTENTSIZE_UNKNOWN || frame_size != expected_logical_bytes) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::invalid_format, "Zstandard frame content size disagrees with the channel record"});
    }

    std::vector<std::byte> decoded(static_cast<std::size_t>(expected_logical_bytes));
    const std::size_t result = ZSTD_decompress(
        decoded.data(), decoded.size(), stored_bytes.data(), stored_bytes.size());
    if (ZSTD_isError(result) != 0) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::decompression_failed,
            std::string{"Zstandard decompression failed: "} + ZSTD_getErrorName(result)});
    }
    if (result != decoded.size()) {
        return Result<std::vector<std::byte>>::failure(
            Error{ErrorCode::decompression_failed, "Zstandard produced an unexpected byte count"});
    }
    return Result<std::vector<std::byte>>::success(std::move(decoded));
}

}  // namespace lunar::terrain
