#include <lunar/terrain/error.hpp>

#include <utility>

namespace lunar::terrain {

std::string_view error_code_name(const ErrorCode code) noexcept {
    switch (code) {
        case ErrorCode::none:
            return "none";
        case ErrorCode::invalid_argument:
            return "invalid_argument";
        case ErrorCode::invalid_tile_key:
            return "invalid_tile_key";
        case ErrorCode::parse_error:
            return "parse_error";
        case ErrorCode::io_error:
            return "io_error";
        case ErrorCode::truncated_data:
            return "truncated_data";
        case ErrorCode::arithmetic_overflow:
            return "arithmetic_overflow";
        case ErrorCode::invalid_format:
            return "invalid_format";
        case ErrorCode::unsupported_version:
            return "unsupported_version";
        case ErrorCode::unsupported_feature:
            return "unsupported_feature";
        case ErrorCode::checksum_mismatch:
            return "checksum_mismatch";
        case ErrorCode::hash_mismatch:
            return "hash_mismatch";
        case ErrorCode::decompression_failed:
            return "decompression_failed";
        case ErrorCode::not_found:
            return "not_found";
        case ErrorCode::internal_error:
            return "internal_error";
    }
    return "unknown";
}

Error::Error(const ErrorCode error_code, std::string error_message)
    : code(error_code), message(std::move(error_message)) {}

Error& Error::with_offset(const std::uint64_t offset) noexcept {
    context.file_offset = offset;
    return *this;
}

Error& Error::with_path(std::string value) {
    context.path = std::move(value);
    return *this;
}

Error& Error::with_tile_key(const std::uint64_t encoded_key) noexcept {
    context.tile_key = encoded_key;
    return *this;
}

Error& Error::with_channel(const std::uint16_t id) noexcept {
    context.channel_id = id;
    return *this;
}

}  // namespace lunar::terrain

