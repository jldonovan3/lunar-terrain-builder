#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <CLI/CLI.hpp>
#include <fmt/format.h>

#include "builder/builder.hpp"
#include "builder/telemetry.hpp"

#if defined(_WIN32)
#include <Windows.h>
#else
#include <unistd.h>
#endif

namespace {

[[nodiscard]] int report_error(const lunar::terrain::Error& error, const bool json) {
    if (json) {
        fmt::print(stderr, "{}\n", lunar::terrain::builder::format_error_json(error));
    } else {
        fmt::print(stderr, "{}: {}", lunar::terrain::error_code_name(error.code), error.message);
        if (!error.context.path.empty()) {
            fmt::print(stderr, " [{}]", error.context.path);
        }
        if (error.context.tile_key) {
            fmt::print(stderr, " [tile={:016x}]", *error.context.tile_key);
        }
        if (error.context.channel_id) {
            fmt::print(stderr, " [channel={}]", *error.context.channel_id);
        }
        fmt::print(stderr, "\n");
    }
    return error.code == lunar::terrain::ErrorCode::cancelled ? 130 : 1;
}

struct CommonCliOptions {
    std::string run_id;
    std::filesystem::path run_state_directory;
    std::filesystem::path event_log;
    std::filesystem::path decoded_cache_directory;
    double progress_interval_seconds{5.0};
    std::uint64_t memory_budget_mib{2'048};
    std::uint64_t decoded_cache_budget_mib{512};
    std::uint64_t scratch_budget_mib{4'096};
    bool resume{};
};

[[nodiscard]] std::uint64_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(GetCurrentProcessId());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

[[nodiscard]] std::string default_run_id(const std::string_view command) {
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return fmt::format("{}-{}-{}", command, process_id(), milliseconds);
}

[[nodiscard]] lunar::terrain::Result<std::uint64_t> mebibytes_to_bytes(
    const std::uint64_t value,
    const std::string_view option) {
    if (value == 0 ||
        value > (std::numeric_limits<std::uint64_t>::max)() /
            lunar::terrain::builder::bytes_per_mebibyte) {
        return lunar::terrain::Result<std::uint64_t>::failure(lunar::terrain::Error{
            lunar::terrain::ErrorCode::invalid_argument,
            fmt::format("{} must be a positive MiB value that fits uint64", option)});
    }
    return lunar::terrain::Result<std::uint64_t>::success(
        value * lunar::terrain::builder::bytes_per_mebibyte);
}

[[nodiscard]] lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>
make_execution_options(
    const std::string_view command,
    const CommonCliOptions& common,
    const std::stop_token cancellation) {
    auto managed = mebibytes_to_bytes(common.memory_budget_mib, "--memory-budget-mib");
    auto decoded = mebibytes_to_bytes(
        common.decoded_cache_budget_mib, "--decoded-cache-budget-mib");
    auto scratch = mebibytes_to_bytes(common.scratch_budget_mib, "--scratch-budget-mib");
    if (!managed || !decoded || !scratch ||
        !std::isfinite(common.progress_interval_seconds) ||
        common.progress_interval_seconds <= 0.0 ||
        common.progress_interval_seconds >
            static_cast<double>((std::numeric_limits<std::int64_t>::max)()) / 1'000.0) {
        if (!managed) {
            return lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>::failure(
                std::move(managed).error());
        }
        if (!decoded) {
            return lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>::failure(
                std::move(decoded).error());
        }
        if (!scratch) {
            return lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>::failure(
                std::move(scratch).error());
        }
        return lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>::failure(
            lunar::terrain::Error{
                lunar::terrain::ErrorCode::invalid_argument,
                "--progress-interval-seconds must be positive and representable"});
    }
    lunar::terrain::builder::ExecutionOptions options;
    options.budgets = {managed.value(), decoded.value(), scratch.value()};
    options.run_id = common.run_id.empty() ? default_run_id(command) : common.run_id;
    options.run_state_directory = common.run_state_directory;
    options.decoded_cache_directory = common.decoded_cache_directory;
    options.progress_interval = std::chrono::milliseconds{
        static_cast<std::int64_t>(std::llround(common.progress_interval_seconds * 1'000.0))};
    options.cancellation = cancellation;
    options.resume = common.resume;
    return lunar::terrain::Result<lunar::terrain::builder::ExecutionOptions>::success(
        std::move(options));
}

class CliProgressSink final : public lunar::terrain::builder::ProgressSink {
public:
    CliProgressSink(
        std::filesystem::path event_log,
        std::filesystem::path run_state_directory)
        : event_log_(std::move(event_log)),
          run_state_directory_(std::move(run_state_directory)) {}

    [[nodiscard]] lunar::terrain::Result<void> Report(
        const lunar::terrain::builder::ProgressEvent& event) override {
        const std::string line = lunar::terrain::builder::format_progress_event(event);
        fmt::print(
            stderr,
            "[{}] {} {} {}/{} ({:.3f}/s) rss={} MiB\n",
            event.command,
            lunar::terrain::builder::progress_state_name(event.state),
            event.phase,
            event.completed_work,
            event.total_work,
            event.work_rate_per_second,
            event.telemetry.resident_memory_bytes /
                lunar::terrain::builder::bytes_per_mebibyte);
        std::fflush(stderr);

        if (!event_log_.empty()) {
            std::error_code filesystem_error;
            if (!event_log_.parent_path().empty()) {
                std::filesystem::create_directories(
                    event_log_.parent_path(), filesystem_error);
            }
            if (filesystem_error) {
                return lunar::terrain::Result<void>::failure(
                    lunar::terrain::Error{
                        lunar::terrain::ErrorCode::io_error,
                        "could not create event-log directory"}
                        .with_path(event_log_.parent_path().string()));
            }
            std::ofstream stream{event_log_, std::ios::binary | std::ios::app};
            if (!stream.is_open()) {
                return lunar::terrain::Result<void>::failure(
                    lunar::terrain::Error{
                        lunar::terrain::ErrorCode::io_error,
                        "could not open progress event log"}
                        .with_path(event_log_.string()));
            }
            stream.write(line.data(), static_cast<std::streamsize>(line.size()));
            stream.flush();
            if (!stream) {
                return lunar::terrain::Result<void>::failure(
                    lunar::terrain::Error{
                        lunar::terrain::ErrorCode::io_error,
                        "could not append progress event"}
                        .with_path(event_log_.string()));
            }
            stream.close();
            if (event.state != lunar::terrain::builder::ProgressState::running) {
                auto synchronized = lunar::terrain::builder::synchronize_file(event_log_);
                if (!synchronized) {
                    return synchronized;
                }
            }
        }
        if (!run_state_directory_.empty()) {
            auto written = lunar::terrain::builder::write_text_file_atomically(
                run_state_directory_ / "progress.json", line);
            if (!written) {
                return written;
            }
        }
        return lunar::terrain::Result<void>::success();
    }

private:
    std::filesystem::path event_log_;
    std::filesystem::path run_state_directory_;
};

volatile std::sig_atomic_t interruption_requested = 0;

extern "C" void request_interruption(const int signal_number) {
    interruption_requested = signal_number;
}

class SignalStopBridge {
public:
    explicit SignalStopBridge(std::stop_source& source)
        : source_(source),
          previous_interrupt_(std::signal(SIGINT, request_interruption)),
          previous_terminate_(std::signal(SIGTERM, request_interruption)),
#if defined(_WIN32) && defined(SIGBREAK)
          previous_break_(std::signal(SIGBREAK, request_interruption)),
#endif
          monitor_([this](const std::stop_token stop) {
              while (!stop.stop_requested()) {
                  if (interruption_requested != 0) {
                      source_.request_stop();
                      return;
                  }
                  std::this_thread::sleep_for(std::chrono::milliseconds{20});
              }
          }) {}

    ~SignalStopBridge() {
        monitor_.request_stop();
        monitor_.join();
        std::signal(SIGINT, previous_interrupt_);
        std::signal(SIGTERM, previous_terminate_);
#if defined(_WIN32) && defined(SIGBREAK)
        std::signal(SIGBREAK, previous_break_);
#endif
    }

    SignalStopBridge(const SignalStopBridge&) = delete;
    SignalStopBridge& operator=(const SignalStopBridge&) = delete;

private:
    std::stop_source& source_;
    using SignalHandler = void (*)(int);
    SignalHandler previous_interrupt_{};
    SignalHandler previous_terminate_{};
#if defined(_WIN32) && defined(SIGBREAK)
    SignalHandler previous_break_{};
#endif
    std::jthread monitor_;
};

[[nodiscard]] lunar::terrain::Result<void> configure_decoded_cache(
    const std::filesystem::path& path) {
    if (path.empty()) {
        return lunar::terrain::Result<void>::success();
    }
    const std::string value = std::filesystem::absolute(path).lexically_normal().string();
#if defined(_WIN32)
    if (_putenv_s("LUNAR_TERRAIN_DECODED_CACHE", value.c_str()) != 0) {
#else
    if (setenv("LUNAR_TERRAIN_DECODED_CACHE", value.c_str(), 1) != 0) {
#endif
        return lunar::terrain::Result<void>::failure(lunar::terrain::Error{
            lunar::terrain::ErrorCode::io_error,
            "could not set the decoded-cache directory for this process"});
    }
    return lunar::terrain::Result<void>::success();
}

template <typename Value, typename Operation>
[[nodiscard]] lunar::terrain::Result<Value> run_observed_command(
    const std::string_view command,
    const CommonCliOptions& common,
    const std::stop_token cancellation,
    Operation&& operation) {
    auto made_options = make_execution_options(command, common, cancellation);
    if (!made_options) {
        return lunar::terrain::Result<Value>::failure(std::move(made_options).error());
    }
    auto cache = configure_decoded_cache(common.decoded_cache_directory);
    if (!cache) {
        return lunar::terrain::Result<Value>::failure(std::move(cache).error());
    }
    CliProgressSink sink{common.event_log, common.run_state_directory};
    auto options = std::move(made_options).value();
    options.progress_sink = &sink;
    lunar::terrain::builder::TelemetryCollector collector{std::string{command}, options};
    options.telemetry = &collector;
    auto started = collector.Start();
    if (!started) {
        return lunar::terrain::Result<Value>::failure(std::move(started).error());
    }
    auto result = std::forward<Operation>(operation)(options);
    const auto terminal_state = result
        ? lunar::terrain::builder::ProgressState::passed
        : result.error().code == lunar::terrain::ErrorCode::cancelled
            ? lunar::terrain::builder::ProgressState::cancelled
            : lunar::terrain::builder::ProgressState::failed;
    auto finished = collector.Finish(
        terminal_state, result ? std::nullopt : std::optional{result.error()});
    if (!finished && result) {
        return lunar::terrain::Result<Value>::failure(std::move(finished).error());
    }
    return result;
}

void apply_local_overrides(
    lunar::terrain::builder::BuilderConfiguration& configuration,
    const std::filesystem::path& output_directory,
    const std::filesystem::path& cache_directory,
    const std::uint32_t threads) {
    if (!output_directory.empty()) {
        configuration.output_directory = std::filesystem::absolute(output_directory).lexically_normal();
    }
    if (!cache_directory.empty()) {
        configuration.cache_directory = std::filesystem::absolute(cache_directory).lexically_normal();
    }
    if (threads != 0) {
        configuration.worker_threads = threads;
    }
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"Build and inspect deterministic lunar terrain databases"};
    app.set_version_flag(
        "--version", fmt::format("lunar-terrain {}", lunar::terrain::builder::version_string()));

    CommonCliOptions common;
    app.add_option("--run-id", common.run_id, "Machine-local logical run identifier");
    app.add_option(
        "--run-state-directory",
        common.run_state_directory,
        "Machine-local atomic progress/checkpoint directory");
    app.add_option("--event-log", common.event_log, "Progress NDJSON event-log path");
    app.add_option(
        "--progress-interval-seconds",
        common.progress_interval_seconds,
        "Maximum seconds between progress snapshots");
    app.add_option(
        "--memory-budget-mib",
        common.memory_budget_mib,
        "Builder-managed memory budget in MiB");
    app.add_option(
        "--decoded-cache-budget-mib",
        common.decoded_cache_budget_mib,
        "Decoded-block cache sub-budget in MiB");
    app.add_option(
        "--scratch-budget-mib",
        common.scratch_budget_mib,
        "Transient scratch budget in MiB");
    app.add_option(
        "--decoded-cache-directory",
        common.decoded_cache_directory,
        "Machine-local decoded-cache directory");
    app.add_flag("--resume", common.resume, "Resume the matching logical run checkpoint");
    // Let subcommands forward unrecognized options to the parent so machine-local
    // observability options work both before and after the subcommand name.
    app.fallthrough();

    std::filesystem::path scan_path;
    bool scan_json = false;
    CLI::App* scan = app.add_subcommand("scan", "Catalog and validate a builder configuration and source");
    scan->add_option("configuration", scan_path, "TOML configuration path")->required();
    scan->add_flag("--json", scan_json, "Emit machine-readable JSON");

    std::filesystem::path plan_path;
    bool plan_json = false;
    CLI::App* plan = app.add_subcommand("plan", "Plan the configured prototype build");
    plan->add_option("configuration", plan_path, "TOML configuration path")->required();
    plan->add_flag("--json", plan_json, "Emit machine-readable JSON");

    std::filesystem::path build_path;
    bool build_json = false;
    bool build_incremental = false;
    std::filesystem::path build_output_directory;
    std::filesystem::path build_cache_directory;
    std::uint32_t build_threads = 0;
    CLI::App* build = app.add_subcommand("build", "Build and transactionally publish LTDB/LTP files");
    build->add_option("configuration", build_path, "TOML configuration path")->required();
    build->add_flag("--json", build_json, "Emit machine-readable JSON");
    build->add_flag(
        "--incremental", build_incremental, "Reuse dependency-identical staged tiles");
    build->add_option(
        "--output-directory", build_output_directory,
        "Machine-local publication directory override");
    build->add_option(
        "--cache-directory", build_cache_directory,
        "Machine-local build-cache directory override");
    build->add_option(
        "--threads", build_threads,
        "Machine-local worker-count override (zero uses configuration)");

    std::filesystem::path validate_path;
    bool validate_full = false;
    bool validate_json = false;
    CLI::App* validate = app.add_subcommand("validate", "Validate an LTDB and its referenced packs");
    validate->add_option("database", validate_path, "LTDB manifest path")->required();
    validate->add_flag("--full", validate_full, "Also verify every same-level seam");
    validate->add_flag("--json", validate_json, "Emit machine-readable JSON");

    std::filesystem::path inspect_path;
    std::string inspect_key;
    bool inspect_json = false;
    CLI::App* inspect = app.add_subcommand("inspect", "Inspect an LTDB or one tile");
    inspect->add_option("database", inspect_path, "LTDB manifest path")->required();
    inspect->add_option("tile", inspect_key, "Optional canonical LunarTileKey");
    inspect->add_flag("--json", inspect_json, "Emit machine-readable JSON");

    std::filesystem::path diff_before_path;
    std::filesystem::path diff_after_path;
    bool diff_json = false;
    CLI::App* diff = app.add_subcommand(
        "diff", "Classify scientific, provenance, dependency, dataset, and package changes");
    diff->add_option("before", diff_before_path, "Baseline LTDB manifest path")->required();
    diff->add_option("after", diff_after_path, "Comparison LTDB manifest path")->required();
    diff->add_flag("--json", diff_json, "Emit machine-readable JSON");

    std::filesystem::path export_path;
    std::filesystem::path export_output;
    std::string export_key;
    std::string export_format;
    CLI::App* export_command = app.add_subcommand(
        "export", "Export mesh, raster, samples, elevation, provenance, or quality diagnostics");
    export_command->add_option("database", export_path, "LTDB manifest path")->required();
    export_command->add_option("tile", export_key, "Canonical LunarTileKey")->required();
    export_command->add_option(
        "--format", export_format,
        "ply, obj, elevation-pgm, csv, raw-u16, provenance-ppm, quality-ppm, or transition-csv")
        ->required();
    export_command->add_option("--output", export_output, "Diagnostic output path")->required();

    std::filesystem::path benchmark_path;
    std::filesystem::path benchmark_output;
    bool benchmark_json = false;
    std::filesystem::path benchmark_output_directory;
    std::filesystem::path benchmark_cache_directory;
    std::uint32_t benchmark_threads = 0;
    CLI::App* benchmark = app.add_subcommand(
        "benchmark", "Record the M7 catalog/build/validation/incremental scale metrics");
    benchmark->add_option("configuration", benchmark_path, "TOML configuration path")->required();
    benchmark->add_option("--output", benchmark_output, "Versioned JSON benchmark report path")
        ->required();
    benchmark->add_flag("--json", benchmark_json, "Emit machine-readable JSON");
    benchmark->add_option(
        "--output-directory", benchmark_output_directory,
        "Machine-local publication directory override");
    benchmark->add_option(
        "--cache-directory", benchmark_cache_directory,
        "Machine-local build-cache directory override");
    benchmark->add_option(
        "--threads", benchmark_threads,
        "Machine-local worker-count override (zero uses configuration)");

    app.require_subcommand(1);
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& error) {
        const int exit_code = app.exit(error);
        return exit_code == 0 ? 0 : 2;
    }

    std::stop_source cancellation;
    SignalStopBridge signal_bridge{cancellation};
    auto valid_common_options = make_execution_options(
        "cli", common, cancellation.get_token());
    if (!valid_common_options) {
        static_cast<void>(report_error(valid_common_options.error(), false));
        return 2;
    }

    using namespace lunar::terrain::builder;
    if (*scan) {
        auto report = run_observed_command<ScanReport>(
            "scan", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                auto configuration = load_configuration(scan_path);
                if (!configuration) {
                    return lunar::terrain::Result<ScanReport>::failure(
                        std::move(configuration).error());
                }
                return scan_configuration(configuration.value(), options);
            });
        if (!report) {
            return report_error(report.error(), scan_json);
        }
        fmt::print("{}", format_report(report.value(), scan_json));
        return 0;
    }
    if (*plan) {
        auto report = run_observed_command<PlanReport>(
            "plan", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                auto configuration = load_configuration(plan_path);
                if (!configuration) {
                    return lunar::terrain::Result<PlanReport>::failure(
                        std::move(configuration).error());
                }
                return plan_configuration(configuration.value(), options);
            });
        if (!report) {
            return report_error(report.error(), plan_json);
        }
        fmt::print("{}", format_report(report.value(), plan_json));
        return 0;
    }
    if (*build) {
        auto report = run_observed_command<BuildReport>(
            "build", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                auto configuration = load_configuration(build_path);
                if (!configuration) {
                    return lunar::terrain::Result<BuildReport>::failure(
                        std::move(configuration).error());
                }
                apply_local_overrides(
                    configuration.value(),
                    build_output_directory,
                    build_cache_directory,
                    build_threads);
                return build_configuration(
                    configuration.value(),
                    BuildOptions{build_incremental, options.cancellation, options});
            });
        if (!report) {
            return report_error(report.error(), build_json);
        }
        fmt::print("{}", format_report(report.value(), build_json));
        return 0;
    }
    if (*validate) {
        auto report = run_observed_command<ValidationReport>(
            "validate", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                return validate_database(validate_path, validate_full, options);
            });
        if (!report) {
            return report_error(report.error(), validate_json);
        }
        fmt::print("{}", format_report(report.value(), validate_json));
        return 0;
    }
    if (*inspect) {
        auto report = run_observed_command<InspectionReport>(
            "inspect", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                std::optional<lunar::terrain::LunarTileKey> key;
                if (!inspect_key.empty()) {
                    auto parsed = lunar::terrain::LunarTileKey::parse(inspect_key);
                    if (!parsed) {
                        return lunar::terrain::Result<InspectionReport>::failure(
                            std::move(parsed).error());
                    }
                    key = parsed.value();
                }
                return inspect_database(inspect_path, key, options);
            });
        if (!report) {
            return report_error(report.error(), inspect_json);
        }
        fmt::print("{}", format_report(report.value(), inspect_json));
        return 0;
    }
    if (*diff) {
        auto report = run_observed_command<DiffReport>(
            "diff", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                return diff_databases(diff_before_path, diff_after_path, options);
            });
        if (!report) {
            return report_error(report.error(), diff_json);
        }
        fmt::print("{}", format_report(report.value(), diff_json));
        return 0;
    }
    if (*export_command) {
        auto exported = run_observed_command<void>(
            "export", common, cancellation.get_token(), [&](const ExecutionOptions& options) {
                auto key = lunar::terrain::LunarTileKey::parse(export_key);
                if (!key) {
                    return lunar::terrain::Result<void>::failure(std::move(key).error());
                }
                std::optional<DiagnosticExportFormat> format;
                if (export_format == "ply") {
                    format = DiagnosticExportFormat::ply;
                } else if (export_format == "obj") {
                    format = DiagnosticExportFormat::obj;
                } else if (export_format == "elevation-pgm" || export_format == "raster") {
                    format = DiagnosticExportFormat::elevation_pgm;
                } else if (export_format == "csv") {
                    format = DiagnosticExportFormat::sample_csv;
                } else if (export_format == "raw-u16") {
                    format = DiagnosticExportFormat::raw_u16_le;
                } else if (export_format == "provenance-ppm" || export_format == "provenance") {
                    format = DiagnosticExportFormat::provenance_ppm;
                } else if (export_format == "quality-ppm" || export_format == "quality") {
                    format = DiagnosticExportFormat::quality_ppm;
                } else if (export_format == "transition-csv") {
                    format = DiagnosticExportFormat::transition_csv;
                }
                if (!format) {
                    return lunar::terrain::Result<void>::failure(lunar::terrain::Error{
                        lunar::terrain::ErrorCode::invalid_argument,
                        "unsupported export format"});
                }
                return export_tile(export_path, key.value(), *format, export_output, options);
            });
        if (!exported) {
            return report_error(exported.error(), false);
        }
        fmt::print("exported {}\n", export_output.string());
        return 0;
    }
    if (*benchmark) {
        auto report = run_observed_command<BenchmarkReport>(
            "benchmark", common, cancellation.get_token(), [&](ExecutionOptions options) {
                options.partial_report_path = benchmark_output;
                auto configuration = load_configuration(benchmark_path);
                if (!configuration) {
                    BenchmarkReport partial;
                    partial.status = configuration.error().code ==
                            lunar::terrain::ErrorCode::cancelled
                        ? "cancelled"
                        : "failed";
                    partial.run_id = options.run_id;
                    partial.active_phase = "configuration";
                    partial.budgets = options.budgets;
                    partial.resumed = options.resume;
                    partial.error = configuration.error();
                    if (options.telemetry != nullptr) {
                        partial.telemetry = options.telemetry->Snapshot();
                    }
                    auto written = write_benchmark_report(partial, benchmark_output);
                    if (!written) {
                        return lunar::terrain::Result<BenchmarkReport>::failure(
                            std::move(written).error());
                    }
                    return lunar::terrain::Result<BenchmarkReport>::failure(
                        std::move(configuration).error());
                }
                apply_local_overrides(
                    configuration.value(),
                    benchmark_output_directory,
                    benchmark_cache_directory,
                    benchmark_threads);
                return benchmark_configuration(configuration.value(), options);
            });
        if (!report) {
            return report_error(report.error(), benchmark_json);
        }
        fmt::print("{}", format_report(report.value(), benchmark_json));
        return 0;
    }
    return 1;
}
