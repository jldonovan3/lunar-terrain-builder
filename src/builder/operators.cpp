#include "builder/builder.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>

#if defined(_WIN32)
#include <Windows.h>
#include <Psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace lunar::terrain::builder {
namespace {

using Clock = std::chrono::steady_clock;

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string encoded{"\""};
    for (const unsigned char character : value) {
        if (character == '"' || character == '\\') {
            encoded.push_back('\\');
        }
        if (character >= 0x20U) {
            encoded.push_back(static_cast<char>(character));
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] const DecodedChannel* find_channel(
    const DecodedTerrainTile& tile,
    const ChannelId id) noexcept {
    const auto found = std::ranges::find_if(tile.channels(), [id](const DecodedChannel& channel) {
        return channel.id() == id;
    });
    return found == tile.channels().end() ? nullptr : &*found;
}

[[nodiscard]] bool same_channel(
    const DecodedTerrainTile& before,
    const DecodedTerrainTile& after,
    const ChannelId id) noexcept {
    const DecodedChannel* lhs = find_channel(before, id);
    const DecodedChannel* rhs = find_channel(after, id);
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return lhs->version() == rhs->version() &&
           lhs->element_type() == rhs->element_type() &&
           lhs->components() == rhs->components() &&
           lhs->width() == rhs->width() &&
           lhs->height() == rhs->height() &&
           lhs->flags() == rhs->flags() &&
           lhs->parameter1() == rhs->parameter1() &&
           std::ranges::equal(lhs->bytes(), rhs->bytes());
}

[[nodiscard]] bool same_pack_layout(
    const std::span<const DatabasePackEntry> before,
    const std::span<const DatabasePackEntry> after) {
    if (before.size() != after.size()) {
        return false;
    }
    for (std::size_t index = 0; index < before.size(); ++index) {
        if (before[index].id != after[index].id ||
            before[index].relative_path.generic_string() !=
                after[index].relative_path.generic_string() ||
            before[index].tile_count != after[index].tile_count ||
            before[index].file_bytes != after[index].file_bytes ||
            before[index].first_tile != after[index].first_tile ||
            before[index].last_tile != after[index].last_tile) {
            return false;
        }
    }
    return true;
}

void diff_dataset_ids(
    const std::span<const DatasetId> before,
    const std::span<const DatasetId> after,
    DiffReport& report) {
    std::size_t before_index = 0;
    std::size_t after_index = 0;
    while (before_index < before.size() || after_index < after.size()) {
        if (after_index == after.size() ||
            (before_index < before.size() && before[before_index] < after[after_index])) {
            report.removed_datasets.push_back(before[before_index++]);
        } else if (before_index == before.size() || after[after_index] < before[before_index]) {
            report.added_datasets.push_back(after[after_index++]);
        } else {
            ++before_index;
            ++after_index;
        }
    }
}

[[nodiscard]] Result<void> compare_matching_tile(
    LunarTerrainDatabase& before_database,
    LunarTerrainDatabase& after_database,
    const TileIndexEntry& before,
    const TileIndexEntry& after,
    DiffReport& report) {
    if (before.dependency_hash_prefix != after.dependency_hash_prefix) {
        report.dependency_changes.push_back(before.key);
    }
    const bool content_changed = before.content_hash_prefix != after.content_hash_prefix;
    if (content_changed) {
        report.content_changes.push_back(before.key);
        auto before_tile = before_database.ReadTile(before.key);
        if (!before_tile) {
            return Result<void>::failure(std::move(before_tile).error());
        }
        auto after_tile = after_database.ReadTile(after.key);
        if (!after_tile) {
            return Result<void>::failure(std::move(after_tile).error());
        }
        if (!same_channel(before_tile.value(), after_tile.value(), ChannelId::provenance)) {
            report.provenance_changes.push_back(before.key);
        }
    }
    if (before.pack_id != after.pack_id ||
        before.payload_offset != after.payload_offset ||
        before.stored_bytes != after.stored_bytes) {
        report.package_layout_changes.push_back(before.key);
    }
    return Result<void>::success();
}

[[nodiscard]] std::string tile_array(const std::span<const LunarTileKey> keys) {
    std::string result;
    for (std::size_t index = 0; index < keys.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += json_string(keys[index].to_string());
    }
    return result;
}

[[nodiscard]] std::string dataset_array(const std::span<const DatasetId> datasets) {
    std::string result;
    for (std::size_t index = 0; index < datasets.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += fmt::format("{}", datasets[index].value);
    }
    return result;
}

[[nodiscard]] double seconds_between(const Clock::time_point start, const Clock::time_point end) {
    return std::chrono::duration<double>{end - start}.count();
}

[[nodiscard]] Result<std::uint64_t> directory_bytes(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (error) {
            return Result<std::uint64_t>::failure(
                Error{ErrorCode::io_error, "could not inspect benchmark staging directory"}
                    .with_path(path.string()));
        }
        return Result<std::uint64_t>::success(0);
    }
    std::uint64_t total = 0;
    std::filesystem::recursive_directory_iterator iterator{
        path, std::filesystem::directory_options::skip_permission_denied, error};
    const std::filesystem::recursive_directory_iterator end;
    while (!error && iterator != end) {
        if (iterator->is_regular_file(error)) {
            const std::uint64_t bytes = iterator->file_size(error);
            if (!error && bytes > (std::numeric_limits<std::uint64_t>::max)() - total) {
                return Result<std::uint64_t>::failure(
                    Error{ErrorCode::arithmetic_overflow, "benchmark staging byte total overflowed"}
                        .with_path(path.string()));
            }
            if (!error) {
                total += bytes;
            }
        }
        iterator.increment(error);
    }
    if (error) {
        return Result<std::uint64_t>::failure(
            Error{ErrorCode::io_error, "could not enumerate benchmark staging artifacts"}
                .with_path(path.string()));
    }
    return Result<std::uint64_t>::success(total);
}

[[nodiscard]] std::uint64_t peak_resident_memory_bytes() noexcept {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) != 0) {
        return static_cast<std::uint64_t>(counters.PeakWorkingSetSize);
    }
#elif defined(__unix__) || defined(__APPLE__)
    struct rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) == 0) {
#if defined(__APPLE__)
        return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
        return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024U;
#endif
    }
#endif
    return 0;
}

[[nodiscard]] bool same_published_identity(
    const BuildReport& clean,
    const BuildReport& incremental) noexcept {
    if (clean.database_content_hash != incremental.database_content_hash ||
        clean.builder_configuration_hash != incremental.builder_configuration_hash ||
        clean.packs.size() != incremental.packs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < clean.packs.size(); ++index) {
        if (clean.packs[index].id != incremental.packs[index].id ||
            clean.packs[index].sha256 != incremental.packs[index].sha256 ||
            clean.packs[index].bytes != incremental.packs[index].bytes) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::string host_platform() {
#if defined(_WIN32)
    return "windows";
#elif defined(__linux__)
    return "linux";
#elif defined(__APPLE__)
    return "macos";
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string compiler_identity() {
#if defined(_MSC_VER)
    return fmt::format("msvc-{}", _MSC_FULL_VER);
#elif defined(__clang__)
    return fmt::format("clang-{}.{}.{}", __clang_major__, __clang_minor__, __clang_patchlevel__);
#elif defined(__GNUC__)
    return fmt::format("gcc-{}.{}.{}", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    return "unknown";
#endif
}

[[nodiscard]] std::string build_configuration() {
#if defined(NDEBUG)
    return "release";
#else
    return "debug";
#endif
}

}  // namespace

bool DiffReport::identical() const noexcept {
    return !dataset_registry_changed && !builder_configuration_changed &&
           !package_layout_changed && added_datasets.empty() && removed_datasets.empty() &&
           added_tiles.empty() && removed_tiles.empty() && dependency_changes.empty() &&
           content_changes.empty() && provenance_changes.empty() &&
           package_layout_changes.empty();
}

Result<DiffReport> diff_databases(
    const std::filesystem::path& before_path,
    const std::filesystem::path& after_path,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase("diff");
    }
    auto execution = check_execution(options);
    if (!execution) {
        return Result<DiffReport>::failure(std::move(execution).error());
    }
    auto before_database = LunarTerrainDatabase::Open(before_path);
    if (!before_database) {
        return Result<DiffReport>::failure(std::move(before_database).error());
    }
    auto after_database = LunarTerrainDatabase::Open(after_path);
    if (!after_database) {
        return Result<DiffReport>::failure(std::move(after_database).error());
    }

    DiffReport report;
    report.before_path = before_path;
    report.after_path = after_path;
    report.dataset_registry_changed =
        before_database.value().Header().dataset_registry_hash !=
        after_database.value().Header().dataset_registry_hash;
    report.builder_configuration_changed =
        before_database.value().Header().builder_configuration_hash !=
        after_database.value().Header().builder_configuration_hash;

    const std::vector<DatasetId> before_datasets = before_database.value().DatasetIds();
    const std::vector<DatasetId> after_datasets = after_database.value().DatasetIds();
    diff_dataset_ids(before_datasets, after_datasets, report);
    const std::vector<DatabasePackEntry> before_packs = before_database.value().Packs();
    const std::vector<DatabasePackEntry> after_packs = after_database.value().Packs();
    report.package_layout_changed = !same_pack_layout(before_packs, after_packs);

    const std::vector<TileIndexEntry> before_tiles = before_database.value().TileIndex();
    const std::vector<TileIndexEntry> after_tiles = after_database.value().TileIndex();
    std::size_t before_index = 0;
    std::size_t after_index = 0;
    while (before_index < before_tiles.size() || after_index < after_tiles.size()) {
        execution = check_execution(options);
        if (!execution) {
            return Result<DiffReport>::failure(std::move(execution).error());
        }
        if (after_index == after_tiles.size() ||
            (before_index < before_tiles.size() &&
             before_tiles[before_index].key < after_tiles[after_index].key)) {
            report.removed_tiles.push_back(before_tiles[before_index++].key);
            report.package_layout_changed = true;
        } else if (before_index == before_tiles.size() ||
                   after_tiles[after_index].key < before_tiles[before_index].key) {
            report.added_tiles.push_back(after_tiles[after_index++].key);
            report.package_layout_changed = true;
        } else {
            auto compared = compare_matching_tile(
                before_database.value(),
                after_database.value(),
                before_tiles[before_index],
                after_tiles[after_index],
                report);
            if (!compared) {
                return Result<DiffReport>::failure(std::move(compared).error());
            }
            ++before_index;
            ++after_index;
        }
        if (options.telemetry != nullptr) {
            options.telemetry->SetWork(
                before_index + after_index,
                before_tiles.size() + after_tiles.size());
        }
    }
    report.package_layout_changed =
        report.package_layout_changed || !report.package_layout_changes.empty();
    return Result<DiffReport>::success(std::move(report));
}

std::string format_report(const DiffReport& report, const bool json) {
    if (json) {
        return fmt::format(
            "{{\"added_dataset_ids\":[{}],\"added_tiles\":[{}],\"after_path\":{},"
            "\"before_path\":{},\"builder_configuration_changed\":{},"
            "\"content_changes\":[{}],\"dataset_registry_changed\":{},"
            "\"dependency_changes\":[{}],\"identical\":{},"
            "\"package_layout_changed\":{},\"package_layout_changes\":[{}],"
            "\"provenance_changes\":[{}],\"removed_dataset_ids\":[{}],"
            "\"removed_tiles\":[{}]}}\n",
            dataset_array(report.added_datasets),
            tile_array(report.added_tiles),
            json_string(report.after_path.string()),
            json_string(report.before_path.string()),
            report.builder_configuration_changed,
            tile_array(report.content_changes),
            report.dataset_registry_changed,
            tile_array(report.dependency_changes),
            report.identical(),
            report.package_layout_changed,
            tile_array(report.package_layout_changes),
            tile_array(report.provenance_changes),
            dataset_array(report.removed_datasets),
            tile_array(report.removed_tiles));
    }
    return fmt::format(
        "before: {}\nafter: {}\nidentical: {}\ndataset registry changed: {}\n"
        "builder configuration changed: {}\npackage layout changed: {}\n"
        "added/removed datasets: {}/{}\nadded/removed tiles: {}/{}\n"
        "dependency changes: {}\nuncompressed content changes: {}\n"
        "provenance changes: {}\npackage placement changes: {}\n",
        report.before_path.string(),
        report.after_path.string(),
        report.identical() ? "yes" : "no",
        report.dataset_registry_changed ? "yes" : "no",
        report.builder_configuration_changed ? "yes" : "no",
        report.package_layout_changed ? "yes" : "no",
        report.added_datasets.size(),
        report.removed_datasets.size(),
        report.added_tiles.size(),
        report.removed_tiles.size(),
        report.dependency_changes.size(),
        report.content_changes.size(),
        report.provenance_changes.size(),
        report.package_layout_changes.size());
}

Result<BenchmarkReport> benchmark_configuration(
    const BuilderConfiguration& configuration,
    const ExecutionOptions& options) {
    BenchmarkReport report;
    report.run_id = options.run_id;
    report.host_platform = host_platform();
    report.compiler = compiler_identity();
    report.build_configuration = build_configuration();
    report.worker_threads = configuration.worker_threads;
    report.budgets = options.budgets;
    report.resumed = options.resume;

    const auto persist = [&]() -> Result<void> {
        if (options.partial_report_path.empty()) {
            return Result<void>::success();
        }
        return write_benchmark_report(report, options.partial_report_path);
    };
    const auto fail = [&](Error error) -> Result<BenchmarkReport> {
        report.status = error.code == ErrorCode::cancelled ? "cancelled" : "failed";
        report.error = error;
        if (options.telemetry != nullptr) {
            report.telemetry = options.telemetry->Snapshot();
        }
        auto written = persist();
        if (!written) {
            return Result<BenchmarkReport>::failure(std::move(written).error());
        }
        return Result<BenchmarkReport>::failure(std::move(error));
    };

    auto written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }

    std::optional<PreparedSourceCatalog> prepared_catalog;
    ExecutionOptions phase_options = options;
    if (configuration.source_kind == BuilderSourceKind::raster) {
        report.active_phase = "source_catalog";
        if (options.telemetry != nullptr) {
            options.telemetry->SetPhase("source_catalog", 0, configuration.rasters.size());
        }
        auto prepared = prepare_source_catalog(configuration, options.telemetry);
        if (!prepared) {
            return fail(std::move(prepared).error());
        }
        prepared_catalog.emplace(std::move(prepared).value());
        phase_options.prepared_source_catalog = &*prepared_catalog;
    }

    report.active_phase = "scan";
    const auto scan_start = Clock::now();
    auto scan = scan_configuration(configuration, phase_options);
    const auto scan_end = Clock::now();
    report.scan_seconds = seconds_between(scan_start, scan_end);
    report.catalog_seconds = report.scan_seconds;
    if (!scan) {
        return fail(std::move(scan).error());
    }
    report.builder_configuration_hash = scan.value().builder_configuration_hash;
    report.scan_complete = true;
    written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }
    if (options.telemetry != nullptr) {
        auto checkpoint = options.telemetry->Checkpoint("benchmark-scan");
        if (!checkpoint) {
            return fail(std::move(checkpoint).error());
        }
    }

    report.active_phase = "plan";
    const auto plan_start = Clock::now();
    auto plan = plan_configuration(configuration, phase_options);
    const auto plan_end = Clock::now();
    report.plan_seconds = seconds_between(plan_start, plan_end);
    if (!plan) {
        return fail(std::move(plan).error());
    }
    report.planned_tile_count = plan.value().expected_hierarchy_tiles.size();
    report.representative_tiles = plan.value().tiles;
    report.plan_level_counts = plan.value().level_counts;
    report.plan_complete = true;
    written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }
    if (options.telemetry != nullptr) {
        auto checkpoint = options.telemetry->Checkpoint("benchmark-plan");
        if (!checkpoint) {
            return fail(std::move(checkpoint).error());
        }
    }

    report.active_phase = "clean_build";
    const auto build_start = Clock::now();
    auto clean = build_configuration(
        configuration,
        BuildOptions{false, phase_options.cancellation, phase_options});
    const auto build_end = Clock::now();
    report.clean_build_seconds = seconds_between(build_start, build_end);
    if (!clean) {
        return fail(std::move(clean).error());
    }
    report.clean_build_complete = true;
    report.database_content_hash = clean.value().database_content_hash;
    report.ordered_pack_hashes.reserve(clean.value().packs.size());
    for (const PackBuildReport& pack : clean.value().packs) {
        report.ordered_pack_hashes.push_back(pack.sha256);
    }
    written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }

    report.active_phase = "validation";
    const auto validation_start = Clock::now();
    auto validation = validate_database(clean.value().database_path, true, phase_options);
    const auto validation_end = Clock::now();
    report.validation_seconds = seconds_between(validation_start, validation_end);
    if (!validation) {
        return fail(std::move(validation).error());
    }
    report.validation_complete = true;
    written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }

    auto staging = directory_bytes(configuration.cache_directory / "staging");
    if (!staging) {
        return fail(std::move(staging).error());
    }

    report.active_phase = "incremental_build";
    const auto incremental_start = Clock::now();
    auto incremental = build_configuration(
        configuration,
        BuildOptions{true, phase_options.cancellation, phase_options});
    const auto incremental_end = Clock::now();
    report.incremental_build_seconds = seconds_between(incremental_start, incremental_end);
    if (!incremental) {
        return fail(std::move(incremental).error());
    }
    report.incremental_build_complete = true;

    report.sampled_core_vertices =
        clean.value().built_tile_count * std::uint64_t{format_v1::core_vertices} *
        format_v1::core_vertices;
    report.staging_io_bytes = staging.value();
    report.peak_resident_memory_bytes = peak_resident_memory_bytes();
    auto database = LunarTerrainDatabase::Open(clean.value().database_path);
    if (!database) {
        return fail(std::move(database).error());
    }
    for (const TileIndexEntry& entry : database.value().TileIndex()) {
        if (entry.logical_channel_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - report.uncompressed_channel_bytes) {
            return fail(Error{
                ErrorCode::arithmetic_overflow,
                "benchmark uncompressed channel byte total overflowed"});
        }
        report.uncompressed_channel_bytes += entry.logical_channel_bytes;
    }
    for (const PackBuildReport& pack : clean.value().packs) {
        report.stored_pack_bytes += pack.bytes;
    }
    report.pack_count = static_cast<std::uint32_t>(clean.value().packs.size());
    report.built_tile_count = clean.value().built_tile_count;
    report.reused_tile_count = incremental.value().reused_tile_count;
    if (report.clean_build_seconds > 0.0) {
        report.sampling_throughput_samples_per_second =
            static_cast<double>(report.sampled_core_vertices) / report.clean_build_seconds;
        report.staging_io_mebibytes_per_second =
            (static_cast<double>(report.staging_io_bytes) / (1024.0 * 1024.0)) /
            report.clean_build_seconds;
    }
    if (report.stored_pack_bytes != 0) {
        report.compression_ratio = static_cast<double>(report.uncompressed_channel_bytes) /
                                   static_cast<double>(report.stored_pack_bytes);
    }
    if (incremental.value().tile_count != 0) {
        report.incremental_reuse_ratio =
            static_cast<double>(report.reused_tile_count) /
            static_cast<double>(incremental.value().tile_count);
    }
    report.deterministic_rebuild = same_published_identity(clean.value(), incremental.value());
    if (!report.deterministic_rebuild) {
        return fail(Error{
            ErrorCode::hash_mismatch,
            "benchmark incremental rebuild did not reproduce the clean published identity"});
    }
    report.status = "passed";
    report.active_phase = "complete";
    if (options.telemetry != nullptr) {
        report.telemetry = options.telemetry->Snapshot();
        const auto source_samples = report.telemetry.named_counts.find("requested_source_samples");
        if (source_samples != report.telemetry.named_counts.end()) {
            report.requested_source_samples = source_samples->second;
        }
        const auto halo_samples = report.telemetry.named_counts.find("requested_halo_samples");
        if (halo_samples != report.telemetry.named_counts.end()) {
            report.requested_halo_samples = halo_samples->second;
        }
    }
    written = persist();
    if (!written) {
        return Result<BenchmarkReport>::failure(std::move(written).error());
    }
    return Result<BenchmarkReport>::success(report);
}

std::string format_report(const BenchmarkReport& report, const bool json) {
    if (json) {
        std::string pack_hashes;
        for (std::size_t index = 0; index < report.ordered_pack_hashes.size(); ++index) {
            if (index != 0) {
                pack_hashes.push_back(',');
            }
            pack_hashes += json_string(report.ordered_pack_hashes[index].to_hex());
        }
        std::string prototype_tiles;
        for (std::size_t index = 0; index < report.representative_tiles.size(); ++index) {
            if (index != 0) {
                prototype_tiles.push_back(',');
            }
            prototype_tiles += json_string(report.representative_tiles[index].to_string());
        }
        std::string level_counts;
        for (std::size_t index = 0; index < report.plan_level_counts.size(); ++index) {
            if (index != 0) {
                level_counts.push_back(',');
            }
            level_counts += fmt::format(
                "{{\"level\":{},\"tile_count\":{}}}",
                report.plan_level_counts[index].level,
                report.plan_level_counts[index].tile_count);
        }
        std::string counts;
        for (const auto& [name, value] : report.telemetry.named_counts) {
            if (!counts.empty()) {
                counts.push_back(',');
            }
            counts += fmt::format("{}:{}", json_string(name), value);
        }
        std::string timings;
        for (const auto& [name, value] : report.telemetry.categorized_seconds) {
            if (!timings.empty()) {
                timings.push_back(',');
            }
            timings += fmt::format("{}:{:.17g}", json_string(name), value);
        }
        const std::string database_hash = report.database_content_hash
            ? json_string(report.database_content_hash->to_hex())
            : "null";
        const std::string error = report.error
            ? format_error_json(*report.error)
            : "null";
        return fmt::format(
            "{{\"benchmark_schema\":{},\"status\":{},\"run_id\":{},\"active_phase\":{},"
            "\"resumed\":{},\"prototype_tiles\":[{}],\"level_counts\":[{}],"
            "\"build_configuration\":{},\"builder_configuration_sha256\":{},"
            "\"database_content_sha256\":{},\"ordered_pack_sha256\":[{}],"
            "\"built_tile_count\":{},\"compiler\":{},"
            "\"catalog_seconds\":{:.17g},\"scan_seconds\":{:.17g},"
            "\"plan_seconds\":{:.17g},\"clean_build_seconds\":{:.17g},"
            "\"compression_ratio\":{:.17g},\"deterministic_rebuild\":{},"
            "\"host_platform\":{},"
            "\"incremental_build_seconds\":{:.17g},\"incremental_reuse_ratio\":{:.17g},"
            "\"pack_count\":{},\"peak_resident_memory_bytes\":{},\"planned_tile_count\":{},"
            "\"reused_tile_count\":{},\"sampled_core_vertices\":{},"
            "\"requested_source_samples\":{},\"requested_halo_samples\":{},"
            "\"sampling_throughput_samples_per_second\":{:.17g},"
            "\"staging_io_bytes\":{},\"staging_io_mebibytes_per_second\":{:.17g},"
            "\"stored_pack_bytes\":{},\"uncompressed_channel_bytes\":{},"
            "\"validation_seconds\":{:.17g},\"worker_threads\":{},"
            "\"phase_complete\":{{\"scan\":{},\"plan\":{},\"clean_build\":{},"
            "\"validation\":{},\"incremental_build\":{}}},"
            "\"budgets\":{{\"managed_memory_bytes\":{},\"decoded_cache_bytes\":{},"
            "\"transient_scratch_bytes\":{}}},"
            "\"resources\":{{\"rss_bytes\":{},\"peak_rss_bytes\":{},"
            "\"commit_bytes\":{},\"page_faults\":{},\"io_read_bytes\":{},"
            "\"io_write_bytes\":{},\"decoded_cache_live_bytes\":{},"
            "\"decoded_cache_hits\":{},\"decoded_cache_misses\":{},"
            "\"decoded_cache_evictions\":{},\"staging_live_bytes\":{},"
            "\"staging_cumulative_bytes\":{},\"staging_high_water_bytes\":{}}},"
            "\"counts\":{{{}}},\"timings_seconds\":{{{}}},\"error\":{}}}\n",
            json_string(report.benchmark_schema),
            json_string(report.status),
            json_string(report.run_id),
            json_string(report.active_phase),
            report.resumed,
            prototype_tiles,
            level_counts,
            json_string(report.build_configuration),
            json_string(report.builder_configuration_hash.to_hex()),
            database_hash,
            pack_hashes,
            report.built_tile_count,
            json_string(report.compiler),
            report.catalog_seconds,
            report.scan_seconds,
            report.plan_seconds,
            report.clean_build_seconds,
            report.compression_ratio,
            report.deterministic_rebuild,
            json_string(report.host_platform),
            report.incremental_build_seconds,
            report.incremental_reuse_ratio,
            report.pack_count,
            report.peak_resident_memory_bytes,
            report.planned_tile_count,
            report.reused_tile_count,
            report.sampled_core_vertices,
            report.requested_source_samples,
            report.requested_halo_samples,
            report.sampling_throughput_samples_per_second,
            report.staging_io_bytes,
            report.staging_io_mebibytes_per_second,
            report.stored_pack_bytes,
            report.uncompressed_channel_bytes,
            report.validation_seconds,
            report.worker_threads,
            report.scan_complete,
            report.plan_complete,
            report.clean_build_complete,
            report.validation_complete,
            report.incremental_build_complete,
            report.budgets.managed_memory_bytes,
            report.budgets.decoded_cache_bytes,
            report.budgets.transient_scratch_bytes,
            report.telemetry.resident_memory_bytes,
            report.telemetry.peak_resident_memory_bytes,
            report.telemetry.committed_memory_bytes,
            report.telemetry.page_faults,
            report.telemetry.io_read_bytes,
            report.telemetry.io_write_bytes,
            report.telemetry.decoded_cache_live_bytes,
            report.telemetry.decoded_cache_hits,
            report.telemetry.decoded_cache_misses,
            report.telemetry.decoded_cache_evictions,
            report.telemetry.staging_live_bytes,
            report.telemetry.staging_cumulative_bytes,
            report.telemetry.staging_high_water_bytes,
            counts,
            timings,
            error);
    }
    return fmt::format(
        "benchmark schema/status: {} / {}\nplatform/compiler/build: {} / {} / {}\n"
        "workers: {}\nconfiguration sha256: {}\n"
        "catalog: {:.6f} s\nclean build: {:.6f} s\nsampling throughput: {:.3f} samples/s\n"
        "staging I/O: {} bytes ({:.3f} MiB/s)\npeak resident memory: {} bytes\n"
        "compression ratio: {:.6f}\npacks: {}\nvalidation: {:.6f} s\n"
        "incremental: {:.6f} s\nreuse: {}/{} ({:.3f}%)\ndeterministic rebuild: {}\n",
        report.benchmark_schema,
        report.status,
        report.host_platform,
        report.compiler,
        report.build_configuration,
        report.worker_threads,
        report.builder_configuration_hash.to_hex(),
        report.catalog_seconds,
        report.clean_build_seconds,
        report.sampling_throughput_samples_per_second,
        report.staging_io_bytes,
        report.staging_io_mebibytes_per_second,
        report.peak_resident_memory_bytes,
        report.compression_ratio,
        report.pack_count,
        report.validation_seconds,
        report.incremental_build_seconds,
        report.reused_tile_count,
        report.built_tile_count,
        report.incremental_reuse_ratio * 100.0,
        report.deterministic_rebuild ? "yes" : "no");
}

Result<void> write_benchmark_report(
    const BenchmarkReport& report,
    const std::filesystem::path& output_path) {
    const std::string text = format_report(report, true);
    return write_text_file_atomically(output_path, text);
}

}  // namespace lunar::terrain::builder
