#include <lunar/terrain/qsc_topology.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain {
namespace {

constexpr std::array<std::array<QscEdgeConnection, 4>, 6> connections{{
    // front
    {{{QscFace::left, QscEdge::east, false},
      {QscFace::right, QscEdge::west, false},
      {QscFace::south, QscEdge::north, false},
      {QscFace::north, QscEdge::south, false}}},
    // right
    {{{QscFace::front, QscEdge::east, false},
      {QscFace::back, QscEdge::west, false},
      {QscFace::south, QscEdge::east, true},
      {QscFace::north, QscEdge::east, false}}},
    // back
    {{{QscFace::right, QscEdge::east, false},
      {QscFace::left, QscEdge::west, false},
      {QscFace::south, QscEdge::south, true},
      {QscFace::north, QscEdge::north, true}}},
    // left
    {{{QscFace::back, QscEdge::east, false},
      {QscFace::front, QscEdge::west, false},
      {QscFace::south, QscEdge::west, false},
      {QscFace::north, QscEdge::west, true}}},
    // north
    {{{QscFace::left, QscEdge::north, true},
      {QscFace::right, QscEdge::north, false},
      {QscFace::front, QscEdge::north, false},
      {QscFace::back, QscEdge::north, true}}},
    // south
    {{{QscFace::left, QscEdge::south, false},
      {QscFace::right, QscEdge::south, true},
      {QscFace::back, QscEdge::south, true},
      {QscFace::front, QscEdge::south, false}}},
}};

[[nodiscard]] constexpr std::size_t index(const QscFace face) noexcept {
    return static_cast<std::size_t>(face);
}

[[nodiscard]] constexpr std::size_t index(const QscEdge edge) noexcept {
    return static_cast<std::size_t>(edge);
}

[[nodiscard]] bool valid_face(const QscFace face) noexcept {
    return index(face) < connections.size();
}

[[nodiscard]] bool valid_edge(const QscEdge edge) noexcept {
    return index(edge) < connections.front().size();
}

[[nodiscard]] bool vertical_edge(const QscEdge edge) noexcept {
    return edge == QscEdge::west || edge == QscEdge::east;
}

}  // namespace

QscEdgeConnection qsc_edge_connection(const QscFace face, const QscEdge edge) noexcept {
    if (!valid_face(face) || !valid_edge(edge)) {
        return {};
    }
    return connections[index(face)][index(edge)];
}

bool qsc_face_owns_edge(const QscFace face, const QscEdge edge) noexcept {
    if (!valid_face(face) || !valid_edge(edge)) {
        return false;
    }
    return index(face) < index(qsc_edge_connection(face, edge).face);
}

QscFace qsc_corner_owner(
    const QscFace face,
    const QscEdge u_edge,
    const QscEdge v_edge) noexcept {
    if (!valid_face(face) || !valid_edge(u_edge) || !valid_edge(v_edge) ||
        !vertical_edge(u_edge) || vertical_edge(v_edge)) {
        return face;
    }
    const QscFace u_neighbor = qsc_edge_connection(face, u_edge).face;
    const QscFace v_neighbor = qsc_edge_connection(face, v_edge).face;
    return static_cast<QscFace>(
        std::min({index(face), index(u_neighbor), index(v_neighbor)}));
}

Result<QscTileNeighbor> qsc_tile_neighbor(const LunarTileKey key, const QscEdge edge) {
    if (!valid_edge(edge)) {
        return Result<QscTileNeighbor>::failure(
            Error{ErrorCode::invalid_argument, "QSC edge is not valid"}.with_tile_key(key.encoded()));
    }

    const std::uint32_t tiles_per_axis = std::uint32_t{1} << key.level();
    std::uint32_t x = key.x();
    std::uint32_t y = key.y();
    QscEdge touching_edge = QscEdge::west;
    bool crosses_face = false;

    switch (edge) {
        case QscEdge::west:
            if (x > 0) {
                --x;
                touching_edge = QscEdge::east;
            } else {
                crosses_face = true;
            }
            break;
        case QscEdge::east:
            if (x + 1U < tiles_per_axis) {
                ++x;
                touching_edge = QscEdge::west;
            } else {
                crosses_face = true;
            }
            break;
        case QscEdge::south:
            if (y > 0) {
                --y;
                touching_edge = QscEdge::north;
            } else {
                crosses_face = true;
            }
            break;
        case QscEdge::north:
            if (y + 1U < tiles_per_axis) {
                ++y;
                touching_edge = QscEdge::south;
            } else {
                crosses_face = true;
            }
            break;
    }

    QscFace neighbor_face = static_cast<QscFace>(key.face());
    bool reversed = false;
    if (crosses_face) {
        const auto connection = qsc_edge_connection(neighbor_face, edge);
        neighbor_face = connection.face;
        touching_edge = connection.edge;
        reversed = connection.reversed;

        std::uint32_t edge_index = vertical_edge(edge) ? y : x;
        if (reversed) {
            edge_index = tiles_per_axis - 1U - edge_index;
        }
        if (vertical_edge(touching_edge)) {
            x = touching_edge == QscEdge::west ? 0U : tiles_per_axis - 1U;
            y = edge_index;
        } else {
            x = edge_index;
            y = touching_edge == QscEdge::south ? 0U : tiles_per_axis - 1U;
        }
    }

    auto neighbor = LunarTileKey::create(
        static_cast<std::uint8_t>(neighbor_face), key.level(), x, y);
    if (!neighbor) {
        return Result<QscTileNeighbor>::failure(std::move(neighbor).error());
    }
    return Result<QscTileNeighbor>::success(
        QscTileNeighbor{neighbor.value(), touching_edge, reversed});
}

}  // namespace lunar::terrain
