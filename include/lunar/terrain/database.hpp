#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {

struct DatabasePackEntry {
    PackId id;
    // Portable path relative to the database manifest's directory.
    std::filesystem::path relative_path;
    std::uint64_t tile_count{};
    std::uint64_t file_bytes{};
    LunarTileKey first_tile;
    LunarTileKey last_tile;
    Sha256Digest sha256;
};

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
    // Registry and index accessors return owning snapshots in canonical order.
    [[nodiscard]] std::vector<DatasetId> DatasetIds() const;
    [[nodiscard]] std::vector<DatabasePackEntry> Packs() const;
    [[nodiscard]] std::vector<TileIndexEntry> TileIndex() const;
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
