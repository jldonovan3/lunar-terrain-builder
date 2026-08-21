#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/qsc_topology.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {
namespace {

TEST_CASE("QSC lattice uses signed coordinates and identical shared boundaries") {
    auto minimum = QscProjection::LatticeCoordinate(0, 0, 8);
    auto maximum = QscProjection::LatticeCoordinate(255, 256, 8);
    REQUIRE(minimum);
    REQUIRE(maximum);
    CHECK(minimum.value() == -1.0);
    CHECK(maximum.value() == 1.0);

    auto left = QscProjection::LatticeCoordinate(146, 256, 8);
    auto right = QscProjection::LatticeCoordinate(147, 0, 8);
    REQUIRE(left);
    REQUIRE(right);
    CHECK(left.value() == right.value());

    CHECK_FALSE(QscProjection::LatticeCoordinate(256, 0, 8));
    CHECK_FALSE(QscProjection::LatticeCoordinate(0, 257, 8));
}

TEST_CASE("QSC round trips deterministic face interiors") {
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
        std::array{0.25, -0.5},
        std::array{-0.75, 0.125},
        std::array{0.6, 0.8},
    };

    for (const auto face : faces) {
        for (const auto& sample : samples) {
            CAPTURE(static_cast<unsigned>(face), sample[0], sample[1]);
            const QscCoordinate projected{face, sample[0], sample[1], 123.5};
            auto geographic = QscProjection::Inverse(projected);
            REQUIRE(geographic);
            auto round_trip = QscProjection::Forward(geographic.value());
            REQUIRE(round_trip);
            CHECK(round_trip.value().face == face);
            CHECK(std::abs(round_trip.value().u - sample[0]) < 2.0e-14);
            CHECK(std::abs(round_trip.value().v - sample[1]) < 2.0e-14);
            CHECK(round_trip.value().elevation_meters == 123.5);
        }
    }
}

TEST_CASE("QSC longitude, poles, face edges, and cube corners have canonical owners") {
    constexpr double pi = std::numbers::pi_v<double>;
    auto wrapped = QscProjection::Forward({0.0, 3.0 * pi, 0.0});
    REQUIRE(wrapped);
    CHECK(wrapped.value().face == QscFace::back);

    auto north = QscProjection::Forward({pi / 2.0, 1.234, 0.0});
    auto south = QscProjection::Forward({-pi / 2.0, -2.345, 0.0});
    REQUIRE(north);
    REQUIRE(south);
    CHECK(north.value().face == QscFace::north);
    CHECK(north.value().u == 0.0);
    CHECK(north.value().v == 0.0);
    CHECK(south.value().face == QscFace::south);

    auto pole = QscProjection::Inverse({QscFace::north, 0.0, 0.0, 0.0});
    REQUIRE(pole);
    CHECK(pole.value().latitude_radians == pi / 2.0);
    CHECK(pole.value().longitude_radians == 0.0);

    const double edge_latitude = pi / 4.0;
    auto edge = QscProjection::Forward({edge_latitude, 0.0, 0.0});
    REQUIRE(edge);
    CHECK(edge.value().face == QscFace::front);
    CHECK(std::abs(edge.value().v - 1.0) < 2.0e-14);

    const double corner_latitude = std::asin(1.0 / std::sqrt(3.0));
    auto corner = QscProjection::Forward({corner_latitude, pi / 4.0, 0.0});
    REQUIRE(corner);
    CHECK(corner.value().face == QscFace::front);

    CHECK_FALSE(QscProjection::Forward(
        {std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0}));
    CHECK_FALSE(QscProjection::Inverse({QscFace::front, 1.01, 0.0, 0.0}));
}

TEST_CASE("QSC face topology is reciprocal and preserves reversal") {
    constexpr std::array faces{
        QscFace::front,
        QscFace::right,
        QscFace::back,
        QscFace::left,
        QscFace::north,
        QscFace::south,
    };
    constexpr std::array edges{
        QscEdge::west,
        QscEdge::east,
        QscEdge::south,
        QscEdge::north,
    };

    for (const auto face : faces) {
        for (const auto edge : edges) {
            CAPTURE(static_cast<unsigned>(face), static_cast<unsigned>(edge));
            const auto connection = qsc_edge_connection(face, edge);
            const auto reciprocal = qsc_edge_connection(connection.face, connection.edge);
            CHECK(reciprocal.face == face);
            CHECK(reciprocal.edge == edge);
            CHECK(reciprocal.reversed == connection.reversed);
            CHECK(qsc_face_owns_edge(face, edge) !=
                  qsc_face_owns_edge(connection.face, connection.edge));
        }
    }

    CHECK(qsc_corner_owner(QscFace::north, QscEdge::west, QscEdge::south) ==
          QscFace::front);
    CHECK(qsc_corner_owner(QscFace::south, QscEdge::east, QscEdge::north) ==
          QscFace::front);
}

TEST_CASE("QSC topology edge mappings identify the same spherical samples") {
    constexpr std::array faces{
        QscFace::front,
        QscFace::right,
        QscFace::back,
        QscFace::left,
        QscFace::north,
        QscFace::south,
    };
    constexpr std::array edges{
        QscEdge::west,
        QscEdge::east,
        QscEdge::south,
        QscEdge::north,
    };
    constexpr std::array parameters{-0.75, -0.1, 0.4, 0.9};

    const auto edge_coordinate = [](const QscFace face, const QscEdge edge, const double parameter) {
        switch (edge) {
            case QscEdge::west:
                return QscCoordinate{face, -1.0, parameter, 0.0};
            case QscEdge::east:
                return QscCoordinate{face, 1.0, parameter, 0.0};
            case QscEdge::south:
                return QscCoordinate{face, parameter, -1.0, 0.0};
            case QscEdge::north:
                return QscCoordinate{face, parameter, 1.0, 0.0};
        }
        return QscCoordinate{};
    };
    const auto unit_vector = [](const LunarGeodeticCoordinate coordinate) {
        const double cosine = std::cos(coordinate.latitude_radians);
        return std::array{
            cosine * std::cos(coordinate.longitude_radians),
            cosine * std::sin(coordinate.longitude_radians),
            std::sin(coordinate.latitude_radians),
        };
    };

    for (const auto face : faces) {
        for (const auto edge : edges) {
            const auto connection = qsc_edge_connection(face, edge);
            for (const double parameter : parameters) {
                CAPTURE(static_cast<unsigned>(face), static_cast<unsigned>(edge), parameter);
                auto source = QscProjection::Inverse(edge_coordinate(face, edge, parameter));
                const double mapped_parameter = connection.reversed ? -parameter : parameter;
                auto destination = QscProjection::Inverse(
                    edge_coordinate(connection.face, connection.edge, mapped_parameter));
                REQUIRE(source);
                REQUIRE(destination);
                const auto source_vector = unit_vector(source.value());
                const auto destination_vector = unit_vector(destination.value());
                for (std::size_t axis = 0; axis < source_vector.size(); ++axis) {
                    CHECK(std::abs(source_vector[axis] - destination_vector[axis]) < 3.0e-14);
                }
            }
        }
    }
}

TEST_CASE("QSC tile neighbors map cross-face indices and reversal") {
    auto right_south = LunarTileKey::create(1, 3, 5, 0);
    REQUIRE(right_south);
    auto south_east = qsc_tile_neighbor(right_south.value(), QscEdge::south);
    REQUIRE(south_east);
    CHECK(south_east.value().key.face() == 5);
    CHECK(south_east.value().key.x() == 7);
    CHECK(south_east.value().key.y() == 2);
    CHECK(south_east.value().touching_edge == QscEdge::east);
    CHECK(south_east.value().reversed);

    auto reciprocal = qsc_tile_neighbor(south_east.value().key, QscEdge::east);
    REQUIRE(reciprocal);
    CHECK(reciprocal.value().key == right_south.value());
    CHECK(reciprocal.value().touching_edge == QscEdge::south);
    CHECK(reciprocal.value().reversed);

    auto interior = LunarTileKey::create(0, 3, 3, 4);
    REQUIRE(interior);
    auto west = qsc_tile_neighbor(interior.value(), QscEdge::west);
    REQUIRE(west);
    CHECK(west.value().key.face() == 0);
    CHECK(west.value().key.x() == 2);
    CHECK(west.value().key.y() == 4);
    CHECK_FALSE(west.value().reversed);
}

}  // namespace
}  // namespace lunar::terrain
