#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <string>
#include <string_view>

#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

struct Sha256Digest {
    std::array<std::byte, 32> bytes{};

    [[nodiscard]] std::string to_hex() const;
    [[nodiscard]] static Result<Sha256Digest> from_hex(std::string_view text);

    auto operator<=>(const Sha256Digest&) const = default;
};

}  // namespace lunar::terrain

