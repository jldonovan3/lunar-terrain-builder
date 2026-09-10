#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/result.hpp>

namespace lunar::terrain::builder {

inline constexpr std::uint64_t bytes_per_mebibyte = 1024U * 1024U;

struct ResourceBudgets {
    std::uint64_t managed_memory_bytes{2'048U * bytes_per_mebibyte};
    std::uint64_t decoded_cache_bytes{512U * bytes_per_mebibyte};
    std::uint64_t transient_scratch_bytes{4'096U * bytes_per_mebibyte};
};

enum class ProgressState : std::uint8_t {
    running,
    checkpoint,
    passed,
    failed,
    cancelled,
};

struct TelemetrySnapshot {
    std::uint64_t resident_memory_bytes{};
    std::uint64_t peak_resident_memory_bytes{};
    std::uint64_t committed_memory_bytes{};
    std::uint64_t page_faults{};
    std::uint64_t io_read_bytes{};
    std::uint64_t io_write_bytes{};
    std::uint64_t decoded_cache_live_bytes{};
    std::uint64_t decoded_cache_hits{};
    std::uint64_t decoded_cache_misses{};
    std::uint64_t decoded_cache_evictions{};
    std::uint64_t staging_live_bytes{};
    std::uint64_t staging_cumulative_bytes{};
    std::uint64_t staging_high_water_bytes{};
    std::map<std::string, std::uint64_t, std::less<>> named_counts;
    std::map<std::string, double, std::less<>> categorized_seconds;
};

struct ProgressEvent {
    std::string schema{"lunar-terrain-progress-v1"};
    std::string run_id;
    std::uint64_t sequence{};
    std::string command;
    std::string phase;
    ProgressState state{ProgressState::running};
    double elapsed_seconds{};
    std::optional<std::uint8_t> face;
    std::optional<std::uint8_t> level;
    std::optional<std::uint64_t> chunk;
    std::uint64_t completed_work{};
    std::uint64_t total_work{};
    double work_rate_per_second{};
    std::uint32_t active_workers{};
    std::uint32_t blocked_workers{};
    std::uint64_t queue_depth{};
    ResourceBudgets budgets;
    TelemetrySnapshot telemetry;
    std::string checkpoint_id;
    std::optional<Error> error;
};

class ProgressSink {
public:
    virtual ~ProgressSink() = default;

    [[nodiscard]] virtual Result<void> Report(const ProgressEvent& event) = 0;
};

class TelemetryCollector;
struct PreparedSourceCatalog;

struct ExecutionOptions {
    ResourceBudgets budgets;
    std::string run_id;
    std::filesystem::path run_state_directory;
    std::filesystem::path decoded_cache_directory;
    std::filesystem::path partial_report_path;
    std::chrono::milliseconds progress_interval{std::chrono::seconds{5}};
    std::stop_token cancellation;
    ProgressSink* progress_sink{};
    TelemetryCollector* telemetry{};
    const PreparedSourceCatalog* prepared_source_catalog{};
    bool resume{};
};

class TelemetryCollector {
public:
    TelemetryCollector(std::string command, const ExecutionOptions& options);
    ~TelemetryCollector();

    TelemetryCollector(const TelemetryCollector&) = delete;
    TelemetryCollector& operator=(const TelemetryCollector&) = delete;

    [[nodiscard]] Result<void> Start();
    [[nodiscard]] Result<void> Check() const;
    [[nodiscard]] Result<void> Checkpoint(std::string checkpoint_id);
    [[nodiscard]] Result<void> Finish(ProgressState state, std::optional<Error> error = std::nullopt);

    void SetPhase(
        std::string phase,
        std::uint64_t completed_work = 0,
        std::uint64_t total_work = 0,
        std::optional<std::uint8_t> face = std::nullopt,
        std::optional<std::uint8_t> level = std::nullopt,
        std::optional<std::uint64_t> chunk = std::nullopt);
    void SetWork(std::uint64_t completed_work, std::uint64_t total_work);
    void SetWorkers(
        std::uint32_t active_workers,
        std::uint32_t blocked_workers,
        std::uint64_t queue_depth);
    void AddCount(std::string_view name, std::uint64_t amount = 1);
    void AddCategorizedSeconds(std::string_view category, double seconds);
    void AddIo(std::uint64_t read_bytes, std::uint64_t write_bytes);
    void SetDecodedCacheLiveBytes(std::uint64_t bytes);
    void AddDecodedCacheLiveBytes(std::uint64_t bytes);
    void RecordDecodedCacheHit();
    void RecordDecodedCacheMiss();
    void RecordDecodedCacheEviction();
    void SetStagingLiveBytes(std::uint64_t bytes);
    void AddStagingLiveBytes(std::uint64_t bytes);
    void AddStagingBytes(std::uint64_t bytes);

    [[nodiscard]] TelemetrySnapshot Snapshot() const;

private:
    [[nodiscard]] Result<void> Emit(
        ProgressState state,
        std::string checkpoint_id = {},
        std::optional<Error> error = std::nullopt);
    void Reporter(std::stop_token stop);

    std::string command_;
    std::string run_id_;
    ResourceBudgets budgets_;
    std::chrono::milliseconds interval_;
    std::stop_token cancellation_;
    ProgressSink* sink_{};
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex state_mutex_;
    mutable std::mutex emit_mutex_;
    std::string phase_{"starting"};
    std::optional<std::uint8_t> face_;
    std::optional<std::uint8_t> level_;
    std::optional<std::uint64_t> chunk_;
    std::uint64_t completed_work_{};
    std::uint64_t total_work_{};
    std::chrono::steady_clock::time_point phase_started_;
    std::uint64_t phase_started_work_{};
    std::uint32_t active_workers_{};
    std::uint32_t blocked_workers_{};
    std::uint64_t queue_depth_{};
    TelemetrySnapshot counters_;
    std::uint64_t sequence_{};
    std::optional<Error> reporting_error_;
    bool started_reporting_{};
    bool finished_{};
    std::jthread reporter_;
    std::condition_variable_any reporter_wakeup_;
};

class TelemetryActivity {
public:
    TelemetryActivity(TelemetryCollector* collector, std::string_view category) noexcept;
    ~TelemetryActivity();

    TelemetryActivity(const TelemetryActivity&) = delete;
    TelemetryActivity& operator=(const TelemetryActivity&) = delete;

private:
    TelemetryCollector* collector_{};
    std::string_view category_;
    std::chrono::steady_clock::time_point started_;
};

[[nodiscard]] std::string_view progress_state_name(ProgressState state) noexcept;
[[nodiscard]] std::string format_error_json(const Error& error);
[[nodiscard]] std::string format_progress_event(const ProgressEvent& event);
[[nodiscard]] Result<void> write_text_file_atomically(
    const std::filesystem::path& path,
    std::string_view text);
[[nodiscard]] Result<void> synchronize_file(const std::filesystem::path& path);

[[nodiscard]] inline TelemetryCollector* telemetry(const ExecutionOptions& options) noexcept {
    return options.telemetry;
}

[[nodiscard]] Result<void> check_execution(const ExecutionOptions& options);

}  // namespace lunar::terrain::builder
