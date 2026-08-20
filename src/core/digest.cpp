#include <lunar/terrain/digest.hpp>

#include <cstdint>

namespace lunar::terrain {
namespace {

constexpr char hex_digits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(const char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

}  // namespace

std::string Sha256Digest::to_hex() const {
    std::string result;
    result.resize(bytes.size() * 2);
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        const auto value = std::to_integer<std::uint8_t>(bytes[index]);
        result[index * 2] = hex_digits[value >> 4];
        result[index * 2 + 1] = hex_digits[value & 0x0FU];
    }
    return result;
}

Result<Sha256Digest> Sha256Digest::from_hex(const std::string_view text) {
    if (text.size() != 64) {
        return Result<Sha256Digest>::failure(
            Error{ErrorCode::parse_error, "a SHA-256 digest must contain exactly 64 hexadecimal digits"});
    }

    Sha256Digest digest;
    for (std::size_t index = 0; index < digest.bytes.size(); ++index) {
        const int high = hex_value(text[index * 2]);
        const int low = hex_value(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return Result<Sha256Digest>::failure(
                Error{ErrorCode::parse_error, "a SHA-256 digest contains a non-hexadecimal character"});
        }
        digest.bytes[index] = static_cast<std::byte>((high << 4) | low);
    }
    return Result<Sha256Digest>::success(digest);
}

}  // namespace lunar::terrain

