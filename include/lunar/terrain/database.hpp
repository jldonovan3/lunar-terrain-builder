#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {

// Read-only owner of a validated LTDB manifest. Index records are returned by
// value. ReadTile returns owning decoded channel storage, so its spans remain
// valid only for the lifetime of that returned tile.
class LunarTerrainDatabase {
public:
    ~LunarTerrainDatabase();
    LunarTerrainDatabase(LunarTerrainDatabase&&) noexcept;
    LunarTerrainDatabase& operator=(LunarTerrainDatabase&&) noexcept;

    LunarTerrainDatabase(const LunarTerrainDatabase&) = delete;
    LunarTerrainDatabase& operator=(const LunarTerrainDatabase&) = delete;

    [[nodiscard]] static Result<LunarTerrainDatabase> Open(const std::filesystem::path& path);

    [[nodiscard]] const DatabaseHeader& Header() const noexcept;
    [[nodiscard]] std::optional<TileIndexEntry> FindTile(LunarTileKey key) const;
    [[nodiscard]] Result<DecodedTerrainTile> ReadTile(LunarTileKey key) const;

    [[nodiscard]] std::vector<TileIndexEntry> Children(LunarTileKey parent) const;
    [[nodiscard]] std::vector<TileIndexEntry> QuerySubtree(LunarTileKey root) const;
    [[nodiscard]] std::vector<TileIndexEntry> QueryLeaves(LunarTileKey root) const;

private:
    struct Impl;
    explicit LunarTerrainDatabase(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace lunar::terrain
