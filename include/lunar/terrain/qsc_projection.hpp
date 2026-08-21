#pragma once

#include <cstdint>

#include <lunar/terrain/coordinates.hpp>
#include <lunar/terrain/result.hpp>

namespace lunar::terrain {

enum class QscFace : std::uint8_t {
    front = 0,
    right = 1,
    back = 2,
    left = 3,
    north = 4,
    south = 5,
};

struct QscCoordinate {
    QscFace face{QscFace::front};
    double u{};
    double v{};
    double elevation_meters{};
};

// Spherical QSC conversion using the six face centers fixed by format v1.
// Longitudes returned by Inverse are normalized to [-pi, pi). At either
// pole, longitude is canonically zero. Points exactly shared by multiple
// faces belong to the lowest numeric Face ID.
class QscProjection {
public:
    [[nodiscard]] static Result<QscCoordinate> Forward(LunarGeodeticCoordinate coordinate);
    [[nodiscard]] static Result<LunarGeodeticCoordinate> Inverse(QscCoordinate coordinate);

    // Computes a tile sample's normalized face coordinate with the signed
    // numerator required by the v1 lattice contract.
    [[nodiscard]] static Result<double> LatticeCoordinate(
        std::uint32_t tile_coordinate,
        std::uint16_t sample_index,
        std::uint8_t level);
};

}  // namespace lunar::terrain
