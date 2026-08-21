#include <catch2/catch_test_macros.hpp>

#include <proj.h>

#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>

#include <lunar/terrain/qsc_projection.hpp>

namespace lunar::terrain::builder {
namespace {

struct ProjDestroy {
    void operator()(PJ* projection) const noexcept { proj_destroy(projection); }
};

using ProjectionPtr = std::unique_ptr<PJ, ProjDestroy>;

[[nodiscard]] ProjectionPtr make_projection(const QscFace face) {
    constexpr std::array definitions{
        "+proj=qsc +lat_0=0 +lon_0=0 +R=1",
        "+proj=qsc +lat_0=0 +lon_0=90 +R=1",
        "+proj=qsc +lat_0=0 +lon_0=180 +R=1",
        "+proj=qsc +lat_0=0 +lon_0=-90 +R=1",
        "+proj=qsc +lat_0=90 +lon_0=0 +R=1",
        "+proj=qsc +lat_0=-90 +lon_0=0 +R=1",
    };
    return ProjectionPtr{proj_create(nullptr, definitions[static_cast<std::size_t>(face)])};
}

[[nodiscard]] double longitude_error(const double first, const double second) {
    return std::abs(std::remainder(first - second, 2.0 * std::numbers::pi_v<double>));
}

TEST_CASE("Core spherical QSC agrees with the pinned PROJ oracle") {
    constexpr std::array faces{
        QscFace::front,
        QscFace::right,
        QscFace::back,
        QscFace::left,
        QscFace::north,
        QscFace::south,
    };
    constexpr std::array samples{
        std::array{0.0, 0.0},
        std::array{0.2, -0.4},
        std::array{-0.65, 0.3},
        std::array{0.75, 0.8},
    };

    for (const auto face : faces) {
        auto projection = make_projection(face);
        REQUIRE(projection != nullptr);
        for (const auto& sample : samples) {
            CAPTURE(static_cast<unsigned>(face), sample[0], sample[1]);
            const PJ_COORD oracle_inverse =
                proj_trans(projection.get(), PJ_INV, proj_coord(sample[0], sample[1], 0.0, 0.0));
            REQUIRE(proj_errno(projection.get()) == 0);

            auto core_inverse = QscProjection::Inverse({face, sample[0], sample[1], 0.0});
            REQUIRE(core_inverse);
            CHECK(std::abs(core_inverse.value().latitude_radians - oracle_inverse.lp.phi) < 2.0e-14);
            if (std::abs(std::abs(core_inverse.value().latitude_radians) -
                         std::numbers::pi_v<double> / 2.0) > 1.0e-14) {
                CHECK(longitude_error(core_inverse.value().longitude_radians, oracle_inverse.lp.lam) <
                      2.0e-14);
            }

            const PJ_COORD oracle_forward = proj_trans(
                projection.get(),
                PJ_FWD,
                proj_coord(
                    core_inverse.value().longitude_radians,
                    core_inverse.value().latitude_radians,
                    0.0,
                    0.0));
            REQUIRE(proj_errno(projection.get()) == 0);
            CHECK(std::abs(oracle_forward.xy.x - sample[0]) < 2.0e-14);
            CHECK(std::abs(oracle_forward.xy.y - sample[1]) < 2.0e-14);
        }
    }
}

}  // namespace
}  // namespace lunar::terrain::builder
