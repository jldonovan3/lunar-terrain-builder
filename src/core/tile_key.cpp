#include <lunar/terrain/tile_key.hpp>

#include <array>
#include <charconv>
#include <iomanip>
#include <locale>
#include <sstream>

namespace lunar::terrain {
namespace {

[[nodiscard]] std::uint64_t interleave(
    const std::uint32_t x,
    const std::uint32_t y,
    const std::uint8_t level) noexcept {
    std::uint64_t result = 0;
    for (std::uint8_t bit = 0; bit < level; ++bit) {
        result |= std::uint64_t{(x >> bit) & 1U} << (2U * bit);
        result |= std::uint64_t{(y >> bit) & 1U} << (2U * bit + 1U);
    }
    return result;
}

[[nodiscard]] bool parse_decimal(const std::string_view text, std::uint32_t& result) noexcept {
    if (text.empty()) {
        return false;
    }
    const auto conversion = std::from_chars(text.data(), text.data() + text.size(), result);
    return conversion.ec == std::errc{} && conversion.ptr == text.data() + text.size();
}

[[nodiscard]] std::array<std::string_view, 5> split_key(const std::string_view text) noexcept {
    std::array<std::string_view, 5> parts{};
    std::size_t begin = 0;
    for (std::size_t index = 0; index < parts.size(); ++index) {
        const std::size_t slash = text.find('/', begin);
        if (index + 1 == parts.size()) {
            if (slash != std::string_view::npos) {
                return {};
            }
            parts[index] = text.substr(begin);
            return parts;
        }
        if (slash == std::string_view::npos) {
            return {};
        }
        parts[index] = text.substr(begin, slash - begin);
        begin = slash + 1;
    }
    return {};
}

}  // namespace

Result<LunarTileKey> LunarTileKey::create(
    const std::uint32_t face,
    const std::uint32_t level,
    const std::uint32_t x,
    const std::uint32_t y) {
    if (face > max_face) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::invalid_tile_key, "tile face must be in the range 0 through 5"});
    }
    if (level > max_level) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::invalid_tile_key, "tile level must be in the range 0 through 28"});
    }

    const std::uint32_t tiles_per_axis = std::uint32_t{1} << level;
    if (x >= tiles_per_axis || y >= tiles_per_axis) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::invalid_tile_key, "tile coordinates must be less than 2^level"});
    }

    const auto encoded_face = static_cast<std::uint8_t>(face);
    const auto encoded_level = static_cast<std::uint8_t>(level);
    const std::uint64_t encoded = (std::uint64_t{encoded_face} << 61) |
                                  (std::uint64_t{encoded_level} << 56) |
                                  interleave(x, y, encoded_level);
    return Result<LunarTileKey>::success(LunarTileKey{encoded});
}

Result<LunarTileKey> LunarTileKey::from_encoded(const std::uint64_t encoded) {
    const auto face = static_cast<std::uint8_t>(encoded >> 61);
    const auto level = static_cast<std::uint8_t>((encoded >> 56) & 0x1FU);
    if (face > max_face || level > max_level) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::invalid_tile_key, "encoded tile face or level is out of range"});
    }

    const std::uint64_t used_mask = level == max_level
                                        ? morton_mask
                                        : ((std::uint64_t{1} << (2U * level)) - 1U);
    if ((encoded & morton_mask & ~used_mask) != 0) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::invalid_tile_key, "encoded tile key has nonzero unused Morton bits"});
    }
    return Result<LunarTileKey>::success(LunarTileKey{encoded});
}

Result<LunarTileKey> LunarTileKey::parse(const std::string_view text) {
    const auto parts = split_key(text);
    if (parts[0] != "QSC" || parts[1].size() != 2 || parts[1][0] != 'F' ||
        parts[2].size() != 3 || parts[2][0] != 'L' || parts[3].size() < 4 ||
        parts[4].size() < 4) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::parse_error, "tile key does not use canonical QSC/Ff/Lll/xxxx/yyyy form"});
    }

    std::uint32_t face = 0;
    std::uint32_t level = 0;
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    if (!parse_decimal(parts[1].substr(1), face) || !parse_decimal(parts[2].substr(1), level) ||
        !parse_decimal(parts[3], x) || !parse_decimal(parts[4], y)) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::parse_error, "tile key contains an invalid decimal field"});
    }

    auto result = create(face, level, x, y);
    if (!result) {
        return result;
    }
    if (result.value().to_string() != text) {
        return Result<LunarTileKey>::failure(
            Error{ErrorCode::parse_error, "tile key spelling is not canonical"});
    }
    return result;
}

std::uint32_t LunarTileKey::x() const noexcept {
    std::uint32_t result = 0;
    for (std::uint8_t bit = 0; bit < level(); ++bit) {
        result |= static_cast<std::uint32_t>((morton() >> (2U * bit)) & 1U) << bit;
    }
    return result;
}

std::uint32_t LunarTileKey::y() const noexcept {
    std::uint32_t result = 0;
    for (std::uint8_t bit = 0; bit < level(); ++bit) {
        result |= static_cast<std::uint32_t>((morton() >> (2U * bit + 1U)) & 1U) << bit;
    }
    return result;
}

std::string LunarTileKey::to_string() const {
    std::ostringstream stream;
    stream.imbue(std::locale::classic());
    stream << "QSC/F" << static_cast<unsigned>(face()) << "/L" << std::setfill('0')
           << std::setw(2) << static_cast<unsigned>(level()) << '/' << std::setw(4) << x() << '/'
           << std::setw(4) << y();
    return stream.str();
}

std::optional<LunarTileKey> LunarTileKey::parent() const noexcept {
    if (level() == 0) {
        return std::nullopt;
    }
    const std::uint64_t encoded_parent = (std::uint64_t{face()} << 61) |
                                         (std::uint64_t{level() - 1U} << 56) |
                                         (morton() >> 2U);
    return LunarTileKey{encoded_parent};
}

Result<std::array<LunarTileKey, 4>> LunarTileKey::children() const {
    if (level() == max_level) {
        return Result<std::array<LunarTileKey, 4>>::failure(
            Error{ErrorCode::invalid_tile_key, "a level-28 tile cannot have a representable child"});
    }

    const auto child_level = static_cast<std::uint8_t>(level() + 1U);
    const std::uint64_t prefix = (std::uint64_t{face()} << 61) |
                                 (std::uint64_t{child_level} << 56) |
                                 (morton() << 2U);
    return Result<std::array<LunarTileKey, 4>>::success(
        {LunarTileKey{prefix}, LunarTileKey{prefix | 1U}, LunarTileKey{prefix | 2U},
         LunarTileKey{prefix | 3U}});
}

}  // namespace lunar::terrain
