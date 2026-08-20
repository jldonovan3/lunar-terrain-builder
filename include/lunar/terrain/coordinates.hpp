#pragma once

namespace lunar::terrain {

inline constexpr double reference_lunar_radius_meters = 1'737'400.0;

struct LunarGeodeticCoordinate {
    double latitude_radians{};
    double longitude_radians{};
    double elevation_meters{};
};

}  // namespace lunar::terrain

