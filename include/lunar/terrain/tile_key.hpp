#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

class LunarTileKey {
public:
    static constexpr std::uint8_t max_face = 5;
    static constexpr std::uint8_t max_level = 28;

    [[nodiscard]] static Result<LunarTileKey> create(
        std::uint32_t face,
        std::uint32_t level,
        std::uint32_t x,
        std::uint32_t y);
    [[nodiscard]] static Result<LunarTileKey> from_encoded(std::uint64_t encoded);
    [[nodiscard]] static Result<LunarTileKey> parse(std::string_view text);

    [[nodiscard]] constexpr std::uint64_t encoded() const noexcept { return encoded_; }
    [[nodiscard]] constexpr std::uint8_t face() const noexcept {
        return static_cast<std::uint8_t>(encoded_ >> 61);
    }
    [[nodiscard]] constexpr std::uint8_t level() const noexcept {
        return static_cast<std::uint8_t>((encoded_ >> 56) & 0x1FU);
    }
    [[nodiscard]] constexpr std::uint64_t morton() const noexcept {
        return encoded_ & morton_mask;
    }

    [[nodiscard]] std::uint32_t x() const noexcept;
    [[nodiscard]] std::uint32_t y() const noexcept;
    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] std::optional<LunarTileKey> parent() const noexcept;
    [[nodiscard]] Result<std::array<LunarTileKey, 4>> children() const;

    auto operator<=>(const LunarTileKey&) const = default;

private:
    static constexpr std::uint64_t morton_mask = (std::uint64_t{1} << 56) - 1;

    explicit constexpr LunarTileKey(std::uint64_t validated) noexcept : encoded_(validated) {}

    std::uint64_t encoded_;
};

}  // namespace lunar::terrain

template <>
struct std::hash<lunar::terrain::LunarTileKey> {
    [[nodiscard]] std::size_t operator()(const lunar::terrain::LunarTileKey& key) const noexcept {
        return std::hash<std::uint64_t>{}(key.encoded());
    }
};
