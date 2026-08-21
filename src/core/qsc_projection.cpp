#include <lunar/terrain/qsc_projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {
namespace {

constexpr double pi = std::numbers::pi_v<double>;
constexpr double half_pi = pi / 2.0;
constexpr double quarter_pi = pi / 4.0;
constexpr double two_pi = 2.0 * pi;
constexpr double inverse_sqrt_two = std::numbers::sqrt2_v<double> / 2.0;
constexpr double center_epsilon = 1.0e-10;
constexpr double boundary_epsilon = 32.0 * std::numeric_limits<double>::epsilon();

enum class Area : std::uint8_t {
    zero,
    one,
    two,
    three,
};

[[nodiscard]] bool valid_face(const QscFace face) noexcept {
    return static_cast<std::uint8_t>(face) <= static_cast<std::uint8_t>(QscFace::south);
}

[[nodiscard]] double normalize_longitude(const double longitude) noexcept {
    double normalized = std::fmod(longitude + pi, two_pi);
    if (normalized < 0.0) {
        normalized += two_pi;
    }
    normalized -= pi;
    return normalized == 0.0 ? 0.0 : normalized;
}

[[nodiscard]] QscFace select_face(
    const double x,
    const double y,
    const double z) noexcept {
    const std::array strengths{x, y, -x, -y, z, -z};
    std::size_t selected = 0;
    for (std::size_t index = 1; index < strengths.size(); ++index) {
        if (strengths[index] > strengths[selected] + boundary_epsilon) {
            selected = index;
        }
    }
    return static_cast<QscFace>(selected);
}

[[nodiscard]] double equatorial_theta(
    const double phi,
    const double y,
    const double x,
    Area& area) noexcept {
    if (phi < center_epsilon) {
        area = Area::zero;
        return 0.0;
    }

    double theta = std::atan2(y, x);
    if (std::abs(theta) <= quarter_pi) {
        area = Area::zero;
    } else if (theta > quarter_pi && theta <= half_pi + quarter_pi) {
        area = Area::one;
        theta -= half_pi;
    } else if (theta > half_pi + quarter_pi || theta <= -(half_pi + quarter_pi)) {
        area = Area::two;
        theta += theta >= 0.0 ? -pi : pi;
    } else {
        area = Area::three;
        theta += half_pi;
    }
    return theta;
}

void polar_angles(
    const QscFace face,
    const double latitude,
    const double longitude,
    double& phi,
    double& theta,
    Area& area) noexcept {
    if (face == QscFace::north) {
        phi = half_pi - latitude;
        if (longitude >= quarter_pi && longitude <= half_pi + quarter_pi) {
            area = Area::zero;
            theta = longitude - half_pi;
        } else if (longitude > half_pi + quarter_pi || longitude <= -(half_pi + quarter_pi)) {
            area = Area::one;
            theta = longitude > 0.0 ? longitude - pi : longitude + pi;
        } else if (longitude > -(half_pi + quarter_pi) && longitude <= -quarter_pi) {
            area = Area::two;
            theta = longitude + half_pi;
        } else {
            area = Area::three;
            theta = longitude;
        }
        return;
    }

    phi = half_pi + latitude;
    if (longitude >= quarter_pi && longitude <= half_pi + quarter_pi) {
        area = Area::zero;
        theta = -longitude + half_pi;
    } else if (longitude < quarter_pi && longitude >= -quarter_pi) {
        area = Area::one;
        theta = -longitude;
    } else if (longitude < -quarter_pi && longitude >= -(half_pi + quarter_pi)) {
        area = Area::two;
        theta = -longitude - half_pi;
    } else {
        area = Area::three;
        theta = longitude > 0.0 ? -longitude + pi : -longitude - pi;
    }
}

[[nodiscard]] Result<QscCoordinate> forward_error(const char* message) {
    return Result<QscCoordinate>::failure(Error{ErrorCode::invalid_argument, message});
}

[[nodiscard]] Result<LunarGeodeticCoordinate> inverse_error(const char* message) {
    return Result<LunarGeodeticCoordinate>::failure(Error{ErrorCode::invalid_argument, message});
}

}  // namespace

Result<QscCoordinate> QscProjection::Forward(LunarGeodeticCoordinate coordinate) {
    if (!std::isfinite(coordinate.latitude_radians) ||
        !std::isfinite(coordinate.longitude_radians) ||
        !std::isfinite(coordinate.elevation_meters)) {
        return forward_error("QSC input coordinates must be finite");
    }
    if (coordinate.latitude_radians < -half_pi || coordinate.latitude_radians > half_pi) {
        return forward_error("QSC latitude must be in the range [-pi/2, pi/2]");
    }

    coordinate.longitude_radians = normalize_longitude(coordinate.longitude_radians);
    if (coordinate.latitude_radians == half_pi) {
        return Result<QscCoordinate>::success(
            QscCoordinate{QscFace::north, 0.0, 0.0, coordinate.elevation_meters});
    }
    if (coordinate.latitude_radians == -half_pi) {
        return Result<QscCoordinate>::success(
            QscCoordinate{QscFace::south, 0.0, 0.0, coordinate.elevation_meters});
    }

    const double sin_latitude = std::sin(coordinate.latitude_radians);
    const double cos_latitude = std::cos(coordinate.latitude_radians);
    const double x = cos_latitude * std::cos(coordinate.longitude_radians);
    const double y = cos_latitude * std::sin(coordinate.longitude_radians);
    const double z = sin_latitude;
    const QscFace face = select_face(x, y, z);

    double phi = 0.0;
    double theta = 0.0;
    Area area = Area::zero;
    if (face == QscFace::north || face == QscFace::south) {
        polar_angles(
            face,
            coordinate.latitude_radians,
            coordinate.longitude_radians,
            phi,
            theta,
            area);
    } else {
        double normal = 0.0;
        double local_x = 0.0;
        double local_y = z;
        switch (face) {
            case QscFace::front:
                normal = x;
                local_x = y;
                break;
            case QscFace::right:
                normal = y;
                local_x = -x;
                break;
            case QscFace::back:
                normal = -x;
                local_x = -y;
                break;
            case QscFace::left:
                normal = -y;
                local_x = x;
                break;
            case QscFace::north:
            case QscFace::south:
                break;
        }
        phi = std::acos(std::clamp(normal, -1.0, 1.0));
        theta = equatorial_theta(phi, local_y, local_x, area);
    }

    double mu = std::atan(
        (12.0 / pi) *
        (theta + std::acos(std::sin(theta) * inverse_sqrt_two) - half_pi));
    const double cos_mu = std::cos(mu);
    const double denominator =
        cos_mu * cos_mu * (1.0 - std::cos(std::atan(1.0 / std::cos(theta))));
    const double radial = std::sqrt(std::max(0.0, (1.0 - std::cos(phi)) / denominator));

    switch (area) {
        case Area::zero:
            break;
        case Area::one:
            mu += half_pi;
            break;
        case Area::two:
            mu += pi;
            break;
        case Area::three:
            mu += pi + half_pi;
            break;
    }

    double u = radial * std::cos(mu);
    double v = radial * std::sin(mu);
    if (std::abs(u) > 1.0 + boundary_epsilon || std::abs(v) > 1.0 + boundary_epsilon) {
        return Result<QscCoordinate>::failure(
            Error{ErrorCode::internal_error, "QSC face selection produced an out-of-face coordinate"});
    }
    u = std::clamp(u, -1.0, 1.0);
    v = std::clamp(v, -1.0, 1.0);
    return Result<QscCoordinate>::success(
        QscCoordinate{face, u, v, coordinate.elevation_meters});
}

Result<LunarGeodeticCoordinate> QscProjection::Inverse(const QscCoordinate coordinate) {
    if (!valid_face(coordinate.face)) {
        return inverse_error("QSC face must be in the range 0 through 5");
    }
    if (!std::isfinite(coordinate.u) || !std::isfinite(coordinate.v) ||
        !std::isfinite(coordinate.elevation_meters)) {
        return inverse_error("QSC input coordinates must be finite");
    }
    if (coordinate.u < -1.0 || coordinate.u > 1.0 || coordinate.v < -1.0 ||
        coordinate.v > 1.0) {
        return inverse_error("QSC face coordinates must be in the range [-1, 1]");
    }

    double mu = std::atan2(coordinate.v, coordinate.u);
    const double nu = std::atan(std::hypot(coordinate.u, coordinate.v));
    Area area = Area::three;
    if (coordinate.u >= 0.0 && coordinate.u >= std::abs(coordinate.v)) {
        area = Area::zero;
    } else if (coordinate.v >= 0.0 && coordinate.v >= std::abs(coordinate.u)) {
        area = Area::one;
        mu -= half_pi;
    } else if (coordinate.u < 0.0 && -coordinate.u >= std::abs(coordinate.v)) {
        area = Area::two;
        mu += mu < 0.0 ? pi : -pi;
    } else {
        mu += half_pi;
    }

    const double t = (pi / 12.0) * std::tan(mu);
    const double theta = std::atan(std::sin(t) / (std::cos(t) - inverse_sqrt_two));
    const double cos_mu = std::cos(mu);
    const double tan_nu = std::tan(nu);
    const double cos_phi = std::clamp(
        1.0 - cos_mu * cos_mu * tan_nu * tan_nu *
                  (1.0 - std::cos(std::atan(1.0 / std::cos(theta)))),
        -1.0,
        1.0);

    double latitude = 0.0;
    double longitude = 0.0;
    if (coordinate.face == QscFace::north) {
        const double phi = std::acos(cos_phi);
        latitude = half_pi - phi;
        switch (area) {
            case Area::zero:
                longitude = theta + half_pi;
                break;
            case Area::one:
                longitude = theta < 0.0 ? theta + pi : theta - pi;
                break;
            case Area::two:
                longitude = theta - half_pi;
                break;
            case Area::three:
                longitude = theta;
                break;
        }
    } else if (coordinate.face == QscFace::south) {
        const double phi = std::acos(cos_phi);
        latitude = phi - half_pi;
        switch (area) {
            case Area::zero:
                longitude = -theta + half_pi;
                break;
            case Area::one:
                longitude = -theta;
                break;
            case Area::two:
                longitude = -theta - half_pi;
                break;
            case Area::three:
                longitude = theta < 0.0 ? -theta - pi : -theta + pi;
                break;
        }
    } else {
        double q = cos_phi;
        double squared = q * q;
        double s = squared >= 1.0 ? 0.0 : std::sqrt(1.0 - squared) * std::sin(theta);
        squared += s * s;
        double r = squared >= 1.0 ? 0.0 : std::sqrt(1.0 - squared);

        double swap = 0.0;
        switch (area) {
            case Area::zero:
                break;
            case Area::one:
                swap = r;
                r = -s;
                s = swap;
                break;
            case Area::two:
                r = -r;
                s = -s;
                break;
            case Area::three:
                swap = r;
                r = s;
                s = -swap;
                break;
        }

        switch (coordinate.face) {
            case QscFace::front:
                break;
            case QscFace::right:
                swap = q;
                q = -r;
                r = swap;
                break;
            case QscFace::back:
                q = -q;
                r = -r;
                break;
            case QscFace::left:
                swap = q;
                q = r;
                r = -swap;
                break;
            case QscFace::north:
            case QscFace::south:
                break;
        }

        latitude = std::acos(std::clamp(-s, -1.0, 1.0)) - half_pi;
        longitude = std::atan2(r, q);
    }

    if (std::abs(std::abs(latitude) - half_pi) <= boundary_epsilon) {
        latitude = std::copysign(half_pi, latitude);
        longitude = 0.0;
    } else {
        longitude = normalize_longitude(longitude);
    }
    return Result<LunarGeodeticCoordinate>::success(
        LunarGeodeticCoordinate{latitude, longitude, coordinate.elevation_meters});
}

Result<double> QscProjection::LatticeCoordinate(
    const std::uint32_t tile_coordinate,
    const std::uint16_t sample_index,
    const std::uint8_t level) {
    if (level > LunarTileKey::max_level) {
        return Result<double>::failure(
            Error{ErrorCode::invalid_argument, "QSC lattice level must be in the range 0 through 28"});
    }
    const std::uint32_t tiles_per_axis = std::uint32_t{1} << level;
    if (tile_coordinate >= tiles_per_axis || sample_index > 256U) {
        return Result<double>::failure(
            Error{ErrorCode::invalid_argument, "QSC lattice tile or sample coordinate is out of range"});
    }

    const std::uint64_t grid_coordinate =
        std::uint64_t{tile_coordinate} * 256U + sample_index;
    const std::uint64_t denominator = std::uint64_t{256} << level;
    const std::int64_t numerator =
        static_cast<std::int64_t>(2U * grid_coordinate) -
        static_cast<std::int64_t>(denominator);
    return Result<double>::success(
        static_cast<double>(numerator) / static_cast<double>(denominator));
}

}  // namespace lunar::terrain
