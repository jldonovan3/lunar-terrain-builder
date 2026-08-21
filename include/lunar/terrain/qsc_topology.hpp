#pragma once

#include <cstdint>

#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {

enum class QscEdge : std::uint8_t {
    west = 0,
    east = 1,
    south = 2,
    north = 3,
};

struct QscEdgeConnection {
    QscFace face{QscFace::front};
    QscEdge edge{QscEdge::west};
    bool reversed{};
};

struct QscTileNeighbor {
    LunarTileKey key;
    QscEdge touching_edge{QscEdge::west};
    bool reversed{};
};

// Returns the adjacent face edge and whether increasing source-edge sample
// order maps to decreasing destination-edge sample order.
[[nodiscard]] QscEdgeConnection qsc_edge_connection(QscFace face, QscEdge edge) noexcept;

// Cross-face edge and cube-corner ownership is deterministic: the lowest
// numeric incident Face ID owns the shared boundary.
[[nodiscard]] bool qsc_face_owns_edge(QscFace face, QscEdge edge) noexcept;
[[nodiscard]] QscFace qsc_corner_owner(QscFace face, QscEdge u_edge, QscEdge v_edge) noexcept;

// Finds the same-level neighbor. Within a face, sample order is preserved;
// across a face boundary, touching_edge and reversed describe the mapping.
[[nodiscard]] Result<QscTileNeighbor> qsc_tile_neighbor(LunarTileKey key, QscEdge edge);

}  // namespace lunar::terrain
