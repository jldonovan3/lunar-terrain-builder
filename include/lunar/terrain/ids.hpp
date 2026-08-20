#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace lunar::terrain {

struct DatasetId {
    std::uint32_t value{};
    auto operator<=>(const DatasetId&) const = default;
};

struct PackId {
    std::uint32_t value{};
    auto operator<=>(const PackId&) const = default;
};

struct DatabaseId {
    std::array<std::byte, 16> bytes{};
    auto operator<=>(const DatabaseId&) const = default;
};

}  // namespace lunar::terrain

