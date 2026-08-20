#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace lunar::terrain {

enum class ErrorCode : std::uint16_t {
    none = 0,
    invalid_argument = 1,
    invalid_tile_key = 2,
    parse_error = 3,
    io_error = 4,
    truncated_data = 5,
    arithmetic_overflow = 6,
    invalid_format = 7,
    unsupported_version = 8,
    unsupported_feature = 9,
    checksum_mismatch = 10,
    hash_mismatch = 11,
    decompression_failed = 12,
    not_found = 13,
    internal_error = 14,
};

[[nodiscard]] std::string_view error_code_name(ErrorCode code) noexcept;

struct ErrorContext {
    std::optional<std::uint64_t> file_offset;
    std::string path;
    std::optional<std::uint64_t> tile_key;
    std::optional<std::uint16_t> channel_id;
};

struct Error {
    ErrorCode code{ErrorCode::none};
    std::string message;
    ErrorContext context;

    Error() = default;
    Error(ErrorCode error_code, std::string error_message);

    Error& with_offset(std::uint64_t offset) noexcept;
    Error& with_path(std::string value);
    Error& with_tile_key(std::uint64_t encoded_key) noexcept;
    Error& with_channel(std::uint16_t id) noexcept;
};

}  // namespace lunar::terrain

