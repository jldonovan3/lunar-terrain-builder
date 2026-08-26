#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>
#include <lunar/terrain/tile_key.hpp>

struct sqlite3;

namespace lunar::terrain::builder {

enum class TileBuildState : std::uint8_t {
    building = 1,
    complete = 2,
};

struct CachedTileRecord {
    LunarTileKey key;
    Sha256Digest dependency_hash;
    Sha256Digest content_hash;
    TileBuildState state{TileBuildState::building};
    std::filesystem::path staging_path;
    std::optional<PackId> previous_pack_id;
    std::optional<std::uint64_t> previous_pack_offset;
};

class BuildCache {
public:
    BuildCache(const BuildCache&) = delete;
    BuildCache& operator=(const BuildCache&) = delete;
    BuildCache(BuildCache&&) noexcept;
    BuildCache& operator=(BuildCache&&) noexcept;
    ~BuildCache();

    [[nodiscard]] static Result<BuildCache> Open(
        const std::filesystem::path& cache_directory);

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] Result<void> RecordDataset(
        DatasetId dataset_id,
        const Sha256Digest& source_hash);

    // Returns a row only when it is complete and the full dependency digest is
    // byte-identical. Callers still validate the referenced disposable artifact.
    [[nodiscard]] Result<std::optional<CachedTileRecord>> FindReusableTile(
        LunarTileKey key,
        const Sha256Digest& dependency_hash) const;

    [[nodiscard]] Result<void> MarkTileBuilding(
        LunarTileKey key,
        const Sha256Digest& dependency_hash,
        const std::filesystem::path& staging_path);

    [[nodiscard]] Result<void> StoreCompletedTile(
        LunarTileKey key,
        const Sha256Digest& dependency_hash,
        const Sha256Digest& content_hash,
        const std::filesystem::path& staging_path);

    [[nodiscard]] Result<void> UpdatePacking(
        LunarTileKey key,
        PackId pack_id,
        std::uint64_t pack_offset);

private:
    struct SqliteDeleter {
        void operator()(sqlite3* database) const noexcept;
    };

    explicit BuildCache(
        std::filesystem::path path,
        std::unique_ptr<sqlite3, SqliteDeleter> database) noexcept;

    std::filesystem::path path_;
    std::unique_ptr<sqlite3, SqliteDeleter> database_;
};

}  // namespace lunar::terrain::builder
