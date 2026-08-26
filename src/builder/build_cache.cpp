#include "builder/build_cache.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <fmt/format.h>
#include <sqlite3.h>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain::builder {
namespace {

using SqliteStatement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

[[nodiscard]] Error cache_error(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key = std::nullopt) {
    Error error{code, std::move(message)};
    error.with_path(path.string());
    if (key) {
        error.with_tile_key(key->encoded());
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key = std::nullopt) {
    return Result<T>::failure(cache_error(code, std::move(message), path, key));
}

[[nodiscard]] Result<void> execute(
    sqlite3* database,
    const std::string_view sql,
    const std::filesystem::path& path) {
    char* raw_message = nullptr;
    const int result = sqlite3_exec(database, sql.data(), nullptr, nullptr, &raw_message);
    const std::string message = raw_message == nullptr ? std::string{} : std::string{raw_message};
    sqlite3_free(raw_message);
    if (result != SQLITE_OK) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error,
            fmt::format("SQLite cache operation failed: {}", message),
            path));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<SqliteStatement> prepare(
    sqlite3* database,
    const std::string_view sql,
    const std::filesystem::path& path) {
    sqlite3_stmt* raw_statement = nullptr;
    const int result = sqlite3_prepare_v2(
        database, sql.data(), static_cast<int>(sql.size()), &raw_statement, nullptr);
    if (result != SQLITE_OK) {
        return failure<SqliteStatement>(
            ErrorCode::io_error,
            fmt::format("could not prepare SQLite cache operation: {}", sqlite3_errmsg(database)),
            path);
    }
    return Result<SqliteStatement>::success(
        SqliteStatement{raw_statement, &sqlite3_finalize});
}

[[nodiscard]] std::array<std::byte, 8> tile_key_bytes(const LunarTileKey key) noexcept {
    std::array<std::byte, 8> bytes{};
    for (std::uint32_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = static_cast<std::byte>(key.encoded() >> (index * 8U));
    }
    return bytes;
}

[[nodiscard]] Result<LunarTileKey> read_tile_key(
    sqlite3_stmt* statement,
    const int column,
    const std::filesystem::path& path) {
    const auto* bytes = static_cast<const std::byte*>(sqlite3_column_blob(statement, column));
    const int byte_count = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr || byte_count != 8) {
        return failure<LunarTileKey>(
            ErrorCode::invalid_format, "SQLite cache contains an invalid TileKey", path);
    }
    std::uint64_t encoded = 0;
    for (std::uint32_t index = 0; index < 8U; ++index) {
        encoded |= std::uint64_t{std::to_integer<std::uint8_t>(bytes[index])} << (index * 8U);
    }
    auto key = LunarTileKey::from_encoded(encoded);
    if (!key) {
        Error error = std::move(key).error();
        error.with_path(path.string());
        return Result<LunarTileKey>::failure(std::move(error));
    }
    return key;
}

[[nodiscard]] Result<Sha256Digest> read_digest(
    sqlite3_stmt* statement,
    const int column,
    const std::filesystem::path& path,
    const LunarTileKey key) {
    const auto* bytes = static_cast<const std::byte*>(sqlite3_column_blob(statement, column));
    const int byte_count = sqlite3_column_bytes(statement, column);
    if (bytes == nullptr || byte_count != 32) {
        return failure<Sha256Digest>(
            ErrorCode::invalid_format,
            "SQLite cache contains an invalid SHA-256 digest",
            path,
            key);
    }
    Sha256Digest digest;
    std::copy_n(bytes, digest.bytes.size(), digest.bytes.begin());
    return Result<Sha256Digest>::success(digest);
}

[[nodiscard]] bool bind_blob(
    sqlite3_stmt* statement,
    const int index,
    const std::span<const std::byte> bytes) noexcept {
    return sqlite3_bind_blob(
        statement,
        index,
        bytes.data(),
        static_cast<int>(bytes.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] bool bind_path(
    sqlite3_stmt* statement,
    const int index,
    const std::filesystem::path& path) noexcept {
    const std::string text = path.generic_string();
    return sqlite3_bind_text(
        statement,
        index,
        text.data(),
        static_cast<int>(text.size()),
        SQLITE_TRANSIENT) == SQLITE_OK;
}

[[nodiscard]] Result<void> step_done(
    sqlite3* database,
    sqlite3_stmt* statement,
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key = std::nullopt) {
    if (sqlite3_step(statement) != SQLITE_DONE) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error,
            fmt::format("SQLite cache update failed: {}", sqlite3_errmsg(database)),
            path,
            key));
    }
    return Result<void>::success();
}

}  // namespace

void BuildCache::SqliteDeleter::operator()(sqlite3* database) const noexcept {
    if (database != nullptr) {
        sqlite3_close(database);
    }
}

BuildCache::BuildCache(
    std::filesystem::path path,
    std::unique_ptr<sqlite3, SqliteDeleter> database) noexcept
    : path_(std::move(path)), database_(std::move(database)) {}

BuildCache::BuildCache(BuildCache&&) noexcept = default;
BuildCache& BuildCache::operator=(BuildCache&&) noexcept = default;
BuildCache::~BuildCache() = default;

Result<BuildCache> BuildCache::Open(const std::filesystem::path& cache_directory) {
    const std::filesystem::path path = cache_directory / "cache.sqlite";
    std::error_code filesystem_error;
    std::filesystem::create_directories(cache_directory, filesystem_error);
    if (filesystem_error) {
        return failure<BuildCache>(
            ErrorCode::io_error,
            fmt::format("could not create build-cache directory: {}", filesystem_error.message()),
            path);
    }

    sqlite3* raw_database = nullptr;
    const int opened = sqlite3_open_v2(
        path.string().c_str(),
        &raw_database,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
        nullptr);
    std::unique_ptr<sqlite3, SqliteDeleter> database{raw_database};
    if (opened != SQLITE_OK || raw_database == nullptr) {
        const std::string message = raw_database == nullptr
            ? "unknown SQLite open failure" : sqlite3_errmsg(raw_database);
        return failure<BuildCache>(
            ErrorCode::io_error,
            fmt::format("could not open build cache: {}", message),
            path);
    }
    sqlite3_busy_timeout(raw_database, 5'000);
    constexpr std::string_view schema =
        "PRAGMA journal_mode=WAL;"
        "PRAGMA synchronous=FULL;"
        "CREATE TABLE IF NOT EXISTS datasets("
        " dataset_id INTEGER PRIMARY KEY NOT NULL,"
        " source_sha256 BLOB NOT NULL CHECK(length(source_sha256)=32)"
        ");"
        "CREATE TABLE IF NOT EXISTS tiles("
        " tile_key BLOB PRIMARY KEY NOT NULL CHECK(length(tile_key)=8),"
        " dependency_sha256 BLOB NOT NULL CHECK(length(dependency_sha256)=32),"
        " content_sha256 BLOB CHECK(content_sha256 IS NULL OR length(content_sha256)=32),"
        " build_state INTEGER NOT NULL CHECK(build_state IN (1,2)),"
        " staging_path TEXT NOT NULL,"
        " previous_pack_id INTEGER,"
        " previous_pack_offset INTEGER"
        ");";
    auto initialized = execute(raw_database, schema, path);
    if (!initialized) {
        return Result<BuildCache>::failure(std::move(initialized).error());
    }
    return Result<BuildCache>::success(BuildCache{path, std::move(database)});
}

Result<void> BuildCache::RecordDataset(
    const DatasetId dataset_id,
    const Sha256Digest& source_hash) {
    auto statement = prepare(
        database_.get(),
        "INSERT INTO datasets(dataset_id,source_sha256) VALUES(?1,?2) "
        "ON CONFLICT(dataset_id) DO UPDATE SET source_sha256=excluded.source_sha256",
        path_);
    if (!statement) {
        return Result<void>::failure(std::move(statement).error());
    }
    if (sqlite3_bind_int64(statement.value().get(), 1, dataset_id.value) != SQLITE_OK ||
        !bind_blob(statement.value().get(), 2, source_hash.bytes)) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error, "could not bind SQLite dataset cache values", path_));
    }
    return step_done(database_.get(), statement.value().get(), path_);
}

Result<std::optional<CachedTileRecord>> BuildCache::FindReusableTile(
    const LunarTileKey key,
    const Sha256Digest& dependency_hash) const {
    auto statement = prepare(
        database_.get(),
        "SELECT tile_key,dependency_sha256,content_sha256,build_state,staging_path,"
        "previous_pack_id,previous_pack_offset FROM tiles "
        "WHERE tile_key=?1 AND dependency_sha256=?2 AND build_state=2",
        path_);
    if (!statement) {
        return Result<std::optional<CachedTileRecord>>::failure(std::move(statement).error());
    }
    const auto key_bytes = tile_key_bytes(key);
    if (!bind_blob(statement.value().get(), 1, key_bytes) ||
        !bind_blob(statement.value().get(), 2, dependency_hash.bytes)) {
        return failure<std::optional<CachedTileRecord>>(
            ErrorCode::io_error, "could not bind SQLite tile lookup values", path_, key);
    }
    const int stepped = sqlite3_step(statement.value().get());
    if (stepped == SQLITE_DONE) {
        return Result<std::optional<CachedTileRecord>>::success(std::nullopt);
    }
    if (stepped != SQLITE_ROW) {
        return failure<std::optional<CachedTileRecord>>(
            ErrorCode::io_error,
            fmt::format("SQLite tile lookup failed: {}", sqlite3_errmsg(database_.get())),
            path_,
            key);
    }
    auto stored_key = read_tile_key(statement.value().get(), 0, path_);
    if (!stored_key) {
        return Result<std::optional<CachedTileRecord>>::failure(std::move(stored_key).error());
    }
    auto stored_dependency = read_digest(statement.value().get(), 1, path_, key);
    auto content = read_digest(statement.value().get(), 2, path_, key);
    if (!stored_dependency || !content) {
        return Result<std::optional<CachedTileRecord>>::failure(
            stored_dependency ? std::move(content).error() : std::move(stored_dependency).error());
    }
    const int state = sqlite3_column_int(statement.value().get(), 3);
    const auto* path_text = reinterpret_cast<const char*>(
        sqlite3_column_text(statement.value().get(), 4));
    const int path_bytes = sqlite3_column_bytes(statement.value().get(), 4);
    if (state != static_cast<int>(TileBuildState::complete) || path_text == nullptr ||
        path_bytes <= 0) {
        return failure<std::optional<CachedTileRecord>>(
            ErrorCode::invalid_format, "SQLite cache contains an invalid completed tile", path_, key);
    }
    CachedTileRecord record{
        stored_key.value(),
        stored_dependency.value(),
        content.value(),
        TileBuildState::complete,
        std::filesystem::path{std::string{path_text, static_cast<std::size_t>(path_bytes)}},
        std::nullopt,
        std::nullopt,
    };
    if (sqlite3_column_type(statement.value().get(), 5) != SQLITE_NULL) {
        const sqlite3_int64 pack_id = sqlite3_column_int64(statement.value().get(), 5);
        if (pack_id < 0 || pack_id > std::numeric_limits<std::uint32_t>::max()) {
            return failure<std::optional<CachedTileRecord>>(
                ErrorCode::invalid_format, "SQLite cache contains an invalid PackID", path_, key);
        }
        record.previous_pack_id = PackId{static_cast<std::uint32_t>(pack_id)};
    }
    if (sqlite3_column_type(statement.value().get(), 6) != SQLITE_NULL) {
        const sqlite3_int64 offset = sqlite3_column_int64(statement.value().get(), 6);
        if (offset < 0) {
            return failure<std::optional<CachedTileRecord>>(
                ErrorCode::invalid_format, "SQLite cache contains an invalid pack offset", path_, key);
        }
        record.previous_pack_offset = static_cast<std::uint64_t>(offset);
    }
    return Result<std::optional<CachedTileRecord>>::success(std::move(record));
}

Result<void> BuildCache::MarkTileBuilding(
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::filesystem::path& staging_path) {
    auto statement = prepare(
        database_.get(),
        "INSERT INTO tiles(tile_key,dependency_sha256,content_sha256,build_state,staging_path,"
        "previous_pack_id,previous_pack_offset) VALUES(?1,?2,NULL,1,?3,NULL,NULL) "
        "ON CONFLICT(tile_key) DO UPDATE SET dependency_sha256=excluded.dependency_sha256,"
        "content_sha256=NULL,build_state=1,staging_path=excluded.staging_path,"
        "previous_pack_id=NULL,previous_pack_offset=NULL",
        path_);
    if (!statement) {
        return Result<void>::failure(std::move(statement).error());
    }
    const auto key_bytes = tile_key_bytes(key);
    if (!bind_blob(statement.value().get(), 1, key_bytes) ||
        !bind_blob(statement.value().get(), 2, dependency_hash.bytes) ||
        !bind_path(statement.value().get(), 3, staging_path)) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error, "could not bind SQLite building-tile values", path_, key));
    }
    return step_done(database_.get(), statement.value().get(), path_, key);
}

Result<void> BuildCache::StoreCompletedTile(
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const Sha256Digest& content_hash,
    const std::filesystem::path& staging_path) {
    auto statement = prepare(
        database_.get(),
        "UPDATE tiles SET content_sha256=?1,build_state=2,staging_path=?2 "
        "WHERE tile_key=?3 AND dependency_sha256=?4",
        path_);
    if (!statement) {
        return Result<void>::failure(std::move(statement).error());
    }
    const auto key_bytes = tile_key_bytes(key);
    if (!bind_blob(statement.value().get(), 1, content_hash.bytes) ||
        !bind_path(statement.value().get(), 2, staging_path) ||
        !bind_blob(statement.value().get(), 3, key_bytes) ||
        !bind_blob(statement.value().get(), 4, dependency_hash.bytes)) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error, "could not bind SQLite completed-tile values", path_, key));
    }
    auto updated = step_done(database_.get(), statement.value().get(), path_, key);
    if (!updated) {
        return updated;
    }
    if (sqlite3_changes(database_.get()) != 1) {
        return Result<void>::failure(cache_error(
            ErrorCode::invalid_argument,
            "completed tile does not match a dependency-identical building row",
            path_,
            key));
    }
    return Result<void>::success();
}

Result<void> BuildCache::UpdatePacking(
    const LunarTileKey key,
    const PackId pack_id,
    const std::uint64_t pack_offset) {
    if (pack_offset > static_cast<std::uint64_t>(std::numeric_limits<sqlite3_int64>::max())) {
        return Result<void>::failure(cache_error(
            ErrorCode::arithmetic_overflow, "pack offset exceeds SQLite integer range", path_, key));
    }
    auto statement = prepare(
        database_.get(),
        "UPDATE tiles SET previous_pack_id=?1,previous_pack_offset=?2 "
        "WHERE tile_key=?3 AND build_state=2",
        path_);
    if (!statement) {
        return Result<void>::failure(std::move(statement).error());
    }
    const auto key_bytes = tile_key_bytes(key);
    if (sqlite3_bind_int64(statement.value().get(), 1, pack_id.value) != SQLITE_OK ||
        sqlite3_bind_int64(
            statement.value().get(), 2, static_cast<sqlite3_int64>(pack_offset)) != SQLITE_OK ||
        !bind_blob(statement.value().get(), 3, key_bytes)) {
        return Result<void>::failure(cache_error(
            ErrorCode::io_error, "could not bind SQLite packing values", path_, key));
    }
    auto updated = step_done(database_.get(), statement.value().get(), path_, key);
    if (!updated) {
        return updated;
    }
    if (sqlite3_changes(database_.get()) != 1) {
        return Result<void>::failure(cache_error(
            ErrorCode::not_found, "cannot update packing for an incomplete tile", path_, key));
    }
    return Result<void>::success();
}

}  // namespace lunar::terrain::builder
