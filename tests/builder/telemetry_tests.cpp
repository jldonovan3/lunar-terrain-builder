#include "builder/telemetry.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace lunar::terrain::builder {
namespace {

class RecordingSink final : public ProgressSink {
public:
    Result<void> Report(const ProgressEvent& event) override {
        std::scoped_lock lock{mutex_};
        events_.push_back(event);
        return Result<void>::success();
    }

    [[nodiscard]] std::vector<ProgressEvent> events() const {
        std::scoped_lock lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<ProgressEvent> events_;
};

class FailingSink final : public ProgressSink {
public:
    Result<void> Report(const ProgressEvent&) override {
        if (++calls_ >= 2) {
            return Result<void>::failure(
                Error{ErrorCode::io_error, "test progress sink failure"});
        }
        return Result<void>::success();
    }

private:
    std::atomic<unsigned> calls_{};
};

TEST_CASE("telemetry emits ordered progress, checkpoints, resources, and terminal state") {
    RecordingSink sink;
    ExecutionOptions options;
    options.run_id = "telemetry-test";
    options.progress_interval = std::chrono::milliseconds{2};
    options.progress_sink = &sink;
    TelemetryCollector collector{"build", options};

    REQUIRE(collector.Start());
    collector.SetPhase("sampling", 0, 400);
    std::vector<std::jthread> workers;
    for (unsigned worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&collector] {
            for (unsigned index = 0; index < 100; ++index) {
                collector.AddCount("requested_source_samples");
                collector.AddStagingBytes(8);
            }
        });
    }
    workers.clear();
    collector.SetWork(400, 400);
    collector.SetWorkers(0, 0, 0);
    collector.SetDecodedCacheLiveBytes(1024);
    collector.RecordDecodedCacheHit();
    collector.RecordDecodedCacheMiss();
    collector.RecordDecodedCacheEviction();
    collector.SetStagingLiveBytes(2048);
    REQUIRE(collector.Checkpoint("chunk-0"));
    REQUIRE(collector.Finish(ProgressState::passed));

    const auto events = sink.events();
    REQUIRE(events.size() >= 4);
    for (std::size_t index = 1; index < events.size(); ++index) {
        CHECK(events[index].sequence == events[index - 1].sequence + 1);
    }
    CHECK(events.front().schema == "lunar-terrain-progress-v1");
    CHECK(events.back().state == ProgressState::passed);
    CHECK(events.back().telemetry.named_counts.at("requested_source_samples") == 400);
    CHECK(events.back().telemetry.staging_cumulative_bytes == 3200);
    CHECK(events.back().telemetry.staging_high_water_bytes == 2048);
    CHECK(events.back().telemetry.decoded_cache_hits == 1);
    CHECK(events.back().telemetry.decoded_cache_misses == 1);
    CHECK(events.back().telemetry.decoded_cache_evictions == 1);

    const std::string encoded = format_progress_event(events.back());
    CHECK(encoded.find("\"schema\":\"lunar-terrain-progress-v1\"") != std::string::npos);
    CHECK(encoded.find("\"state\":\"passed\"") != std::string::npos);
    CHECK(encoded.find("\"high_water_bytes\":2048") != std::string::npos);

    ProgressEvent failed_event;
    failed_event.error = Error{ErrorCode::truncated_data, "short tile payload"}
        .with_path("Moon.ltp")
        .with_offset(512)
        .with_tile_key(0x1234)
        .with_channel(1);
    const std::string failed_encoded = format_progress_event(failed_event);
    CHECK(failed_encoded.find("\"file_offset\":512") != std::string::npos);
    CHECK(failed_encoded.find("\"tile_key\":\"0000000000001234\"") != std::string::npos);
    CHECK(failed_encoded.find("\"channel_id\":1") != std::string::npos);
}

TEST_CASE("telemetry surfaces reporting failure and cooperative cancellation") {
    FailingSink sink;
    ExecutionOptions reporting_options;
    reporting_options.progress_sink = &sink;
    TelemetryCollector collector{"scan", reporting_options};
    REQUIRE(collector.Start());
    collector.SetPhase("source_hashing");
    const auto checked = collector.Check();
    REQUIRE_FALSE(checked);
    CHECK(checked.error().code == ErrorCode::io_error);

    std::stop_source cancellation;
    cancellation.request_stop();
    ExecutionOptions cancelled_options;
    cancelled_options.cancellation = cancellation.get_token();
    const auto cancelled = check_execution(cancelled_options);
    REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().code == ErrorCode::cancelled);
}

TEST_CASE("atomic report replacement leaves only the complete destination") {
    const auto path = std::filesystem::temp_directory_path() /
        "lunar-terrain-telemetry-atomic-test.json";
    REQUIRE(write_text_file_atomically(path, "{\"generation\":1}\n"));
    REQUIRE(write_text_file_atomically(path, "{\"generation\":2}\n"));
    std::ifstream stream{path, std::ios::binary};
    const std::string contents{
        std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
    CHECK(contents == "{\"generation\":2}\n");
    CHECK_FALSE(std::filesystem::exists(path.string() + ".tmp"));
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

}  // namespace
}  // namespace lunar::terrain::builder
