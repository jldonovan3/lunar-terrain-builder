#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/tile_key.hpp>

#include "builder/build_cache.hpp"
#include "builder/task_executor.hpp"

namespace lunar::terrain::builder {
namespace {

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::high_resolution_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
            ("lunar-terrain-m6-cache-" + std::to_string(suffix));
        std::filesystem::create_directories(path_);
    }

    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] Sha256Digest digest(const std::byte value) {
    Sha256Digest result;
    result.bytes.fill(value);
    return result;
}

TEST_CASE("SQLite build cache reuses only complete dependency-identical tiles") {
    TemporaryDirectory temporary;
    auto cache = BuildCache::Open(temporary.path());
    REQUIRE(cache);
    CHECK(cache.value().path() == temporary.path() / "cache.sqlite");
    CHECK(std::filesystem::is_regular_file(cache.value().path()));

    const auto key = LunarTileKey::create(5, 12, 123, 456).value();
    const Sha256Digest source = digest(std::byte{0x11});
    const Sha256Digest dependency = digest(std::byte{0x22});
    const Sha256Digest content = digest(std::byte{0x33});
    const auto staging_path = temporary.path() / "staging" / "tile.core-v1";
    REQUIRE(cache.value().RecordDataset(DatasetId{7}, source));
    REQUIRE(cache.value().MarkTileBuilding(key, dependency, staging_path));
    auto building = cache.value().FindReusableTile(key, dependency);
    REQUIRE(building);
    CHECK_FALSE(building.value());

    REQUIRE(cache.value().StoreCompletedTile(key, dependency, content, staging_path));
    REQUIRE(cache.value().UpdatePacking(key, PackId{4}, 8'192));
    auto reusable = cache.value().FindReusableTile(key, dependency);
    REQUIRE(reusable);
    REQUIRE(reusable.value());
    CHECK(reusable.value()->key == key);
    CHECK(reusable.value()->content_hash == content);
    CHECK(reusable.value()->staging_path == staging_path);
    CHECK(reusable.value()->previous_pack_id == PackId{4});
    CHECK(reusable.value()->previous_pack_offset == 8'192);

    auto changed = cache.value().FindReusableTile(key, digest(std::byte{0x44}));
    REQUIRE(changed);
    CHECK_FALSE(changed.value());

    const auto neighbor = LunarTileKey::create(5, 12, 124, 456).value();
    const Sha256Digest neighbor_dependency = digest(std::byte{0x55});
    REQUIRE(cache.value().MarkTileBuilding(
        neighbor, neighbor_dependency, temporary.path() / "staging" / "neighbor.core-v1"));
    REQUIRE(cache.value().StoreCompletedTile(
        neighbor,
        neighbor_dependency,
        digest(std::byte{0x66}),
        temporary.path() / "staging" / "neighbor.core-v1"));
    REQUIRE(cache.value().MarkTileBuilding(
        key, digest(std::byte{0x44}), staging_path));
    auto unaffected = cache.value().FindReusableTile(neighbor, neighbor_dependency);
    REQUIRE(unaffected);
    CHECK(unaffected.value().has_value());
    auto invalidated = cache.value().FindReusableTile(key, dependency);
    REQUIRE(invalidated);
    CHECK_FALSE(invalidated.value());
}

TEST_CASE("bounded tasks honor limits and cancellation") {
    std::atomic<std::uint32_t> active{0};
    std::atomic<std::uint32_t> maximum{0};
    std::atomic<std::uint32_t> completed{0};
    std::vector<BuilderTask> tasks;
    for (std::uint32_t index = 0; index < 12; ++index) {
        tasks.emplace_back([&]() {
            const std::uint32_t now = active.fetch_add(1) + 1U;
            std::uint32_t observed = maximum.load();
            while (observed < now && !maximum.compare_exchange_weak(observed, now)) {
            }
            std::this_thread::yield();
            active.fetch_sub(1);
            completed.fetch_add(1);
            return Result<void>::success();
        });
    }
    auto run = run_bounded_tasks(tasks, 3);
    REQUIRE(run);
    CHECK(completed == tasks.size());
    CHECK(maximum <= 3);

    std::stop_source stopped;
    stopped.request_stop();
    auto cancelled_run = run_bounded_tasks(tasks, 2, stopped.get_token());
    REQUIRE_FALSE(cancelled_run);
    CHECK(cancelled_run.error().code == ErrorCode::cancelled);
}

}  // namespace
}  // namespace lunar::terrain::builder
