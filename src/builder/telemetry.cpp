#include "builder/telemetry.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <system_error>
#include <thread>
#include <utility>

#include <fmt/format.h>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace lunar::terrain::builder {
namespace {

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() + 2U);
    encoded.push_back('"');
    constexpr char digits[] = "0123456789abcdef";
    for (const unsigned char character : value) {
        switch (character) {
            case '"': encoded += "\\\""; break;
            case '\\': encoded += "\\\\"; break;
            case '\b': encoded += "\\b"; break;
            case '\f': encoded += "\\f"; break;
            case '\n': encoded += "\\n"; break;
            case '\r': encoded += "\\r"; break;
            case '\t': encoded += "\\t"; break;
            default:
                if (character < 0x20U) {
                    encoded += "\\u00";
                    encoded.push_back(digits[character >> 4U]);
                    encoded.push_back(digits[character & 0x0FU]);
                } else {
                    encoded.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] TelemetrySnapshot process_snapshot() noexcept {
    TelemetrySnapshot snapshot;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
            sizeof(memory)) != 0) {
        snapshot.resident_memory_bytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
        snapshot.peak_resident_memory_bytes =
            static_cast<std::uint64_t>(memory.PeakWorkingSetSize);
        snapshot.committed_memory_bytes = static_cast<std::uint64_t>(memory.PagefileUsage);
        snapshot.page_faults = static_cast<std::uint64_t>(memory.PageFaultCount);
    }
    IO_COUNTERS io{};
    if (GetProcessIoCounters(GetCurrentProcess(), &io) != 0) {
        snapshot.io_read_bytes = io.ReadTransferCount;
        snapshot.io_write_bytes = io.WriteTransferCount;
    }
#elif defined(__unix__) || defined(__APPLE__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        snapshot.peak_resident_memory_bytes = static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        snapshot.peak_resident_memory_bytes = static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
        snapshot.resident_memory_bytes = snapshot.peak_resident_memory_bytes;
        snapshot.page_faults = static_cast<std::uint64_t>(usage.ru_majflt);
        snapshot.io_read_bytes = static_cast<std::uint64_t>(usage.ru_inblock) * 512U;
        snapshot.io_write_bytes = static_cast<std::uint64_t>(usage.ru_oublock) * 512U;
    }
#endif
    return snapshot;
}

template <typename Value>
void append_numeric_map(
    std::string& output,
    const std::map<std::string, Value, std::less<>>& values) {
    bool first = true;
    for (const auto& [name, value] : values) {
        if (!first) {
            output.push_back(',');
        }
        first = false;
        output += fmt::format("{}:{}", json_string(name), value);
    }
}

[[nodiscard]] std::filesystem::path temporary_sibling(
    const std::filesystem::path& path) {
    return path.parent_path() / (path.filename().string() + ".tmp");
}

[[nodiscard]] Error cancelled_error() {
    return Error{ErrorCode::cancelled, "operation was cancelled"};
}

}  // namespace

std::string_view progress_state_name(const ProgressState state) noexcept {
    switch (state) {
        case ProgressState::running: return "running";
        case ProgressState::checkpoint: return "checkpoint";
        case ProgressState::passed: return "passed";
        case ProgressState::failed: return "failed";
        case ProgressState::cancelled: return "cancelled";
    }
    return "failed";
}

std::string format_error_json(const Error& error) {
    const std::string file_offset = error.context.file_offset
        ? fmt::format("{}", *error.context.file_offset)
        : "null";
    const std::string tile_key = error.context.tile_key
        ? json_string(fmt::format("{:016x}", *error.context.tile_key))
        : "null";
    const std::string channel_id = error.context.channel_id
        ? fmt::format("{}", *error.context.channel_id)
        : "null";
    return fmt::format(
        "{{\"code\":{},\"message\":{},\"context\":{{\"path\":{},"
        "\"file_offset\":{},\"tile_key\":{},\"channel_id\":{}}}}}",
        json_string(error_code_name(error.code)),
        json_string(error.message),
        json_string(error.context.path),
        file_offset,
        tile_key,
        channel_id);
}

std::string format_progress_event(const ProgressEvent& event) {
    std::string counts;
    append_numeric_map(counts, event.telemetry.named_counts);
    std::string timings;
    append_numeric_map(timings, event.telemetry.categorized_seconds);
    const auto optional_u8 = [](const std::optional<std::uint8_t> value) {
        return value ? fmt::format("{}", *value) : std::string{"null"};
    };
    const auto optional_u64 = [](const std::optional<std::uint64_t> value) {
        return value ? fmt::format("{}", *value) : std::string{"null"};
    };
    std::string error{"null"};
    if (event.error) {
        error = format_error_json(*event.error);
    }
    return fmt::format(
        "{{\"schema\":{},\"run_id\":{},\"sequence\":{},\"command\":{},"
        "\"phase\":{},\"state\":{},\"elapsed_seconds\":{:.17g},"
        "\"context\":{{\"face\":{},\"level\":{},\"chunk\":{}}},"
        "\"work\":{{\"completed\":{},\"total\":{},\"rate_per_second\":{:.17g}}},"
        "\"workers\":{{\"active\":{},\"blocked\":{},\"queue_depth\":{}}},"
        "\"resources\":{{\"rss_bytes\":{},\"peak_rss_bytes\":{},"
        "\"commit_bytes\":{},\"page_faults\":{}}},"
        "\"cache\":{{\"resident_bytes\":{},\"hits\":{},\"misses\":{},"
        "\"evictions\":{}}},"
        "\"io\":{{\"read_bytes\":{},\"write_bytes\":{}}},"
        "\"staging\":{{\"live_bytes\":{},\"cumulative_bytes\":{},"
        "\"high_water_bytes\":{}}},"
        "\"budgets\":{{\"managed_memory_bytes\":{},\"decoded_cache_bytes\":{},"
        "\"transient_scratch_bytes\":{}}},"
        "\"counts\":{{{}}},\"timings_seconds\":{{{}}},\"checkpoint_id\":{},"
        "\"error\":{}}}\n",
        json_string(event.schema),
        json_string(event.run_id),
        event.sequence,
        json_string(event.command),
        json_string(event.phase),
        json_string(progress_state_name(event.state)),
        event.elapsed_seconds,
        optional_u8(event.face),
        optional_u8(event.level),
        optional_u64(event.chunk),
        event.completed_work,
        event.total_work,
        event.work_rate_per_second,
        event.active_workers,
        event.blocked_workers,
        event.queue_depth,
        event.telemetry.resident_memory_bytes,
        event.telemetry.peak_resident_memory_bytes,
        event.telemetry.committed_memory_bytes,
        event.telemetry.page_faults,
        event.telemetry.decoded_cache_live_bytes,
        event.telemetry.decoded_cache_hits,
        event.telemetry.decoded_cache_misses,
        event.telemetry.decoded_cache_evictions,
        event.telemetry.io_read_bytes,
        event.telemetry.io_write_bytes,
        event.telemetry.staging_live_bytes,
        event.telemetry.staging_cumulative_bytes,
        event.telemetry.staging_high_water_bytes,
        event.budgets.managed_memory_bytes,
        event.budgets.decoded_cache_bytes,
        event.budgets.transient_scratch_bytes,
        counts,
        timings,
        json_string(event.checkpoint_id),
        error);
}

TelemetryCollector::TelemetryCollector(
    std::string command,
    const ExecutionOptions& options)
    : command_(std::move(command)),
      run_id_(options.run_id.empty() ? "local" : options.run_id),
      budgets_(options.budgets),
      interval_((std::max)(options.progress_interval, std::chrono::milliseconds{1})),
      cancellation_(options.cancellation),
      sink_(options.progress_sink),
      started_(std::chrono::steady_clock::now()),
      phase_started_(started_) {}

TelemetryCollector::~TelemetryCollector() {
    if (reporter_.joinable()) {
        reporter_.request_stop();
        reporter_wakeup_.notify_all();
        reporter_.join();
    }
}

Result<void> TelemetryCollector::Start() {
    {
        std::scoped_lock lock{state_mutex_};
        if (started_reporting_) {
            return Result<void>::failure(Error{
                ErrorCode::invalid_argument, "telemetry collector was started more than once"});
        }
        started_reporting_ = true;
    }
    auto emitted = Emit(ProgressState::running);
    if (!emitted) {
        return emitted;
    }
    if (sink_ != nullptr) {
        reporter_ = std::jthread([this](const std::stop_token stop) { Reporter(stop); });
    }
    return Result<void>::success();
}

Result<void> TelemetryCollector::Check() const {
    if (cancellation_.stop_requested()) {
        return Result<void>::failure(cancelled_error());
    }
    std::scoped_lock lock{state_mutex_};
    if (reporting_error_) {
        return Result<void>::failure(*reporting_error_);
    }
    return Result<void>::success();
}

Result<void> TelemetryCollector::Checkpoint(std::string checkpoint_id) {
    auto checked = Check();
    if (!checked) {
        return checked;
    }
    return Emit(ProgressState::checkpoint, std::move(checkpoint_id));
}

Result<void> TelemetryCollector::Finish(
    const ProgressState state,
    std::optional<Error> error) {
    {
        std::scoped_lock lock{state_mutex_};
        if (finished_) {
            if (reporting_error_) {
                return Result<void>::failure(*reporting_error_);
            }
            return Result<void>::success();
        }
        finished_ = true;
    }
    if (reporter_.joinable()) {
        reporter_.request_stop();
        reporter_wakeup_.notify_all();
        reporter_.join();
    }
    return Emit(state, {}, std::move(error));
}

void TelemetryCollector::SetPhase(
    std::string phase,
    const std::uint64_t completed_work,
    const std::uint64_t total_work,
    const std::optional<std::uint8_t> face,
    const std::optional<std::uint8_t> level,
    const std::optional<std::uint64_t> chunk) {
    {
        std::scoped_lock lock{state_mutex_};
        phase_ = std::move(phase);
        completed_work_ = completed_work;
        total_work_ = total_work;
        phase_started_ = std::chrono::steady_clock::now();
        phase_started_work_ = completed_work;
        face_ = face;
        level_ = level;
        chunk_ = chunk;
    }
    static_cast<void>(Emit(ProgressState::running));
}

void TelemetryCollector::SetWork(
    const std::uint64_t completed_work,
    const std::uint64_t total_work) {
    std::scoped_lock lock{state_mutex_};
    completed_work_ = completed_work;
    total_work_ = total_work;
}

void TelemetryCollector::SetWorkers(
    const std::uint32_t active_workers,
    const std::uint32_t blocked_workers,
    const std::uint64_t queue_depth) {
    std::scoped_lock lock{state_mutex_};
    active_workers_ = active_workers;
    blocked_workers_ = blocked_workers;
    queue_depth_ = queue_depth;
}

void TelemetryCollector::AddCount(const std::string_view name, const std::uint64_t amount) {
    std::scoped_lock lock{state_mutex_};
    auto& value = counters_.named_counts[std::string{name}];
    value = amount > (std::numeric_limits<std::uint64_t>::max)() - value
        ? (std::numeric_limits<std::uint64_t>::max)()
        : value + amount;
}

void TelemetryCollector::AddCategorizedSeconds(
    const std::string_view category,
    const double seconds) {
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return;
    }
    std::scoped_lock lock{state_mutex_};
    counters_.categorized_seconds[std::string{category}] += seconds;
}

void TelemetryCollector::AddIo(
    const std::uint64_t read_bytes,
    const std::uint64_t write_bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.io_read_bytes += read_bytes;
    counters_.io_write_bytes += write_bytes;
}

void TelemetryCollector::SetDecodedCacheLiveBytes(const std::uint64_t bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.decoded_cache_live_bytes = bytes;
}

void TelemetryCollector::AddDecodedCacheLiveBytes(const std::uint64_t bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.decoded_cache_live_bytes =
        bytes > (std::numeric_limits<std::uint64_t>::max)() -
                    counters_.decoded_cache_live_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : counters_.decoded_cache_live_bytes + bytes;
}

void TelemetryCollector::RecordDecodedCacheHit() {
    std::scoped_lock lock{state_mutex_};
    ++counters_.decoded_cache_hits;
}

void TelemetryCollector::RecordDecodedCacheMiss() {
    std::scoped_lock lock{state_mutex_};
    ++counters_.decoded_cache_misses;
}

void TelemetryCollector::RecordDecodedCacheEviction() {
    std::scoped_lock lock{state_mutex_};
    ++counters_.decoded_cache_evictions;
}

void TelemetryCollector::SetStagingLiveBytes(const std::uint64_t bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.staging_live_bytes = bytes;
    counters_.staging_high_water_bytes =
        (std::max)(counters_.staging_high_water_bytes, bytes);
}

void TelemetryCollector::AddStagingLiveBytes(const std::uint64_t bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.staging_live_bytes =
        bytes > (std::numeric_limits<std::uint64_t>::max)() - counters_.staging_live_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : counters_.staging_live_bytes + bytes;
    counters_.staging_high_water_bytes =
        (std::max)(counters_.staging_high_water_bytes, counters_.staging_live_bytes);
}

void TelemetryCollector::AddStagingBytes(const std::uint64_t bytes) {
    std::scoped_lock lock{state_mutex_};
    counters_.staging_cumulative_bytes =
        bytes > (std::numeric_limits<std::uint64_t>::max)() -
                    counters_.staging_cumulative_bytes
        ? (std::numeric_limits<std::uint64_t>::max)()
        : counters_.staging_cumulative_bytes + bytes;
}

TelemetrySnapshot TelemetryCollector::Snapshot() const {
    TelemetrySnapshot result;
    {
        std::scoped_lock lock{state_mutex_};
        result = counters_;
    }
    const TelemetrySnapshot process = process_snapshot();
    result.resident_memory_bytes = process.resident_memory_bytes;
    result.peak_resident_memory_bytes = process.peak_resident_memory_bytes;
    result.committed_memory_bytes = process.committed_memory_bytes;
    result.page_faults = process.page_faults;
    result.io_read_bytes = (std::max)(result.io_read_bytes, process.io_read_bytes);
    result.io_write_bytes = (std::max)(result.io_write_bytes, process.io_write_bytes);
    return result;
}

Result<void> TelemetryCollector::Emit(
    const ProgressState state,
    std::string checkpoint_id,
    std::optional<Error> error) {
    if (sink_ == nullptr) {
        return Result<void>::success();
    }
    std::scoped_lock emit_lock{emit_mutex_};
    ProgressEvent event;
    {
        std::scoped_lock state_lock{state_mutex_};
        if (reporting_error_) {
            return Result<void>::failure(*reporting_error_);
        }
        event.run_id = run_id_;
        event.sequence = ++sequence_;
        event.command = command_;
        event.phase = phase_;
        event.state = state;
        event.elapsed_seconds = std::chrono::duration<double>{
            std::chrono::steady_clock::now() - started_}.count();
        event.face = face_;
        event.level = level_;
        event.chunk = chunk_;
        event.completed_work = completed_work_;
        event.total_work = total_work_;
        const double phase_seconds = std::chrono::duration<double>{
            std::chrono::steady_clock::now() - phase_started_}.count();
        if (phase_seconds > 0.0 && completed_work_ >= phase_started_work_) {
            event.work_rate_per_second =
                static_cast<double>(completed_work_ - phase_started_work_) / phase_seconds;
        }
        event.active_workers = active_workers_;
        event.blocked_workers = blocked_workers_;
        event.queue_depth = queue_depth_;
        event.budgets = budgets_;
        event.checkpoint_id = std::move(checkpoint_id);
        event.error = std::move(error);
    }
    event.telemetry = Snapshot();
    auto reported = sink_->Report(event);
    if (!reported) {
        std::scoped_lock state_lock{state_mutex_};
        reporting_error_ = reported.error();
        return Result<void>::failure(*reporting_error_);
    }
    return Result<void>::success();
}

void TelemetryCollector::Reporter(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::unique_lock lock{state_mutex_};
        reporter_wakeup_.wait_for(lock, stop, interval_, [] { return false; });
        const bool stopped = stop.stop_requested();
        lock.unlock();
        if (stopped) {
            return;
        }
        if (!Emit(ProgressState::running)) {
            return;
        }
    }
}

TelemetryActivity::TelemetryActivity(
    TelemetryCollector* const collector,
    const std::string_view category) noexcept
    : collector_(collector), category_(category), started_(std::chrono::steady_clock::now()) {}

TelemetryActivity::~TelemetryActivity() {
    if (collector_ != nullptr) {
        collector_->AddCategorizedSeconds(
            category_,
            std::chrono::duration<double>{std::chrono::steady_clock::now() - started_}.count());
    }
}

Result<void> synchronize_file(const std::filesystem::path& path) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(
        path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, "could not open file for synchronization"}
                .with_path(path.string()));
    }
    const BOOL flushed = FlushFileBuffers(handle);
    const DWORD error = flushed == 0 ? GetLastError() : ERROR_SUCCESS;
    CloseHandle(handle);
    if (flushed == 0) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, fmt::format("could not synchronize file ({})", error)}
                .with_path(path.string()));
    }
#elif defined(__unix__) || defined(__APPLE__)
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, fmt::format("could not open file for synchronization: {}", std::strerror(errno))}
                .with_path(path.string()));
    }
    const int synchronized = ::fsync(descriptor);
    const int error = errno;
    ::close(descriptor);
    if (synchronized != 0) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, fmt::format("could not synchronize file: {}", std::strerror(error))}
                .with_path(path.string()));
    }
#else
    static_cast<void>(path);
#endif
    return Result<void>::success();
}

Result<void> write_text_file_atomically(
    const std::filesystem::path& path,
    const std::string_view text) {
    std::error_code filesystem_error;
    if (!path.parent_path().empty()) {
        std::filesystem::create_directories(path.parent_path(), filesystem_error);
        if (filesystem_error) {
            return Result<void>::failure(
                Error{ErrorCode::io_error, "could not create report directory"}
                    .with_path(path.parent_path().string()));
        }
    }
    const std::filesystem::path temporary = temporary_sibling(path);
    std::ofstream stream{temporary, std::ios::binary | std::ios::trunc};
    if (!stream.is_open()) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, "could not create temporary report"}
                .with_path(temporary.string()));
    }
    stream.write(text.data(), static_cast<std::streamsize>(text.size()));
    stream.flush();
    if (!stream) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, "could not write temporary report"}
                .with_path(temporary.string()));
    }
    stream.close();
    auto synchronized = synchronize_file(temporary);
    if (!synchronized) {
        return synchronized;
    }
#if defined(_WIN32)
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    DWORD replace_error = ERROR_SUCCESS;
    for (;;) {
        if (MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0) {
            break;
        }
        replace_error = GetLastError();
        const bool retryable = replace_error == ERROR_ACCESS_DENIED ||
            replace_error == ERROR_SHARING_VIOLATION || replace_error == ERROR_LOCK_VIOLATION;
        if (!retryable || std::chrono::steady_clock::now() >= deadline) {
            return Result<void>::failure(
                Error{
                    ErrorCode::io_error,
                    fmt::format("could not atomically replace report ({})", replace_error)}
                    .with_path(path.string()));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{10});
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        return Result<void>::failure(
            Error{ErrorCode::io_error, fmt::format("could not atomically replace report: {}", std::strerror(errno))}
                .with_path(path.string()));
    }
#endif
    return Result<void>::success();
}

Result<void> check_execution(const ExecutionOptions& options) {
    if (options.cancellation.stop_requested()) {
        return Result<void>::failure(cancelled_error());
    }
    if (options.telemetry != nullptr) {
        return options.telemetry->Check();
    }
    return Result<void>::success();
}

}  // namespace lunar::terrain::builder
