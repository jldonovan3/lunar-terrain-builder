#include "builder/builder.hpp"
#include "builder/build_cache.hpp"
#include "builder/fusion.hpp"
#include "builder/hierarchy.hpp"
#include "builder/packing.hpp"
#include "builder/task_executor.hpp"
#include "builder/tile_staging.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <zstd.h>

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/integrity.hpp>
#include <lunar/terrain/qsc_projection.hpp>
#include <lunar/terrain/qsc_topology.hpp>

namespace lunar::terrain::builder {
namespace {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

constexpr std::uint32_t mandatory_chunk_flags = 0x0003U;
constexpr std::uint32_t required_channel = 0x0001U;
constexpr std::uint32_t tile_has_provenance = 0x0001U;
constexpr std::uint32_t tile_has_quality = 0x0002U;
constexpr std::uint32_t synthetic_resolution_meters = 10'000U;

struct ChannelArtifact {
    ChannelId id{ChannelId::elevation};
    ElementType element_type{ElementType::u16};
    Codec codec{Codec::zstandard};
    Predictor predictor{Predictor::none};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint32_t flags{};
    std::uint32_t parameter1{};
    Bytes decoded;
    Bytes logical;
    Bytes stored;
};

struct EncodedTile {
    LunarTileKey key;
    Bytes payload;
    std::uint16_t minimum_code{};
    std::uint16_t maximum_code{};
    Sha256Digest dependency_hash;
    Sha256Digest content_hash;
    std::uint32_t payload_crc{};
    std::uint32_t logical_channel_bytes{};
    std::uint32_t effective_resolution_millimeters{};
    std::uint32_t geometric_error_millimeters{};
    std::uint32_t flags{};
    DatasetId primary_dataset;
    std::uint16_t provenance_palette_count{};
    std::uint8_t materialized_child_mask{};
    std::uint8_t channel_count{};
    std::filesystem::path artifact_path;
    std::uint32_t payload_bytes{};
};

struct RasterTileRecord {
    LunarTileKey key;
    Sha256Digest dependency_hash;
    std::filesystem::path staged_core_path;
    std::filesystem::path auxiliary_path;
    std::filesystem::path quantized_core_path;
    std::optional<EncodedTile> reused_tile;
};

struct TileBuildResult {
    std::vector<EncodedTile> tiles;
    std::uint64_t built_tile_count{};
    std::uint64_t reused_tile_count{};
};

struct RasterTileWork {
    StagedElevationTile staged;
    FusionTileSummary summary;
    ElevationSampler apron_sampler;
    std::vector<double> virtual_apron_samples;
    std::optional<EncodedTile> reused_tile;
};

struct RasterTileAuxiliary {
    FusionTileSummary summary;
    std::vector<double> virtual_apron_samples;
};

struct PackTilePlacement {
    std::size_t tile_index{};
    std::uint64_t payload_offset{};
    std::uint32_t payload_bytes{};
};

struct PackArtifact {
    PackId id;
    std::filesystem::path relative_path;
    Bytes bytes;
    Sha256Digest hash;
    LunarTileKey first_key;
    LunarTileKey last_key;
    std::vector<PackTilePlacement> placements;
    std::uint64_t file_bytes{};
};

struct ChunkArtifact {
    std::array<char, 4> tag{};
    Bytes bytes;
    std::uint64_t file_offset{};
};

[[nodiscard]] std::filesystem::path encoded_tile_artifact_path(
    const BuilderConfiguration& configuration,
    LunarTileKey key,
    const Sha256Digest& dependency_hash);
[[nodiscard]] Result<void> persist_encoded_tile_artifact(
    const std::filesystem::path& path,
    const EncodedTile& tile);
[[nodiscard]] Result<EncodedTile> load_encoded_tile_artifact(
    const std::filesystem::path& path,
    LunarTileKey expected_key,
    const Sha256Digest& expected_dependency_hash,
    const Sha256Digest& expected_content_hash);
[[nodiscard]] Result<void> write_file_synced(
    const std::filesystem::path& path,
    ByteView bytes,
    std::stop_token cancellation = {});
[[nodiscard]] std::filesystem::path temporary_sibling(
    const std::filesystem::path& final_path);
void record_staging_file(
    TelemetryCollector* collector,
    const std::filesystem::path& path);

struct ZstdContextDeleter {
    void operator()(ZSTD_CCtx* context) const noexcept {
        ZSTD_freeCCtx(context);
    }
};

[[nodiscard]] Error build_error(
    const ErrorCode code,
    std::string message,
    const std::optional<std::filesystem::path>& path = std::nullopt) {
    Error error{code, std::move(message)};
    if (path) {
        error.with_path(path->string());
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::optional<std::filesystem::path>& path = std::nullopt) {
    return Result<T>::failure(build_error(code, std::move(message), path));
}

[[nodiscard]] Result<void> check_build_execution(const BuildOptions& options) {
    if (options.cancellation_token().stop_requested()) {
        return Result<void>::failure(
            build_error(ErrorCode::cancelled, "build operation was cancelled"));
    }
    if (options.execution.telemetry != nullptr) {
        return options.execution.telemetry->Check();
    }
    return Result<void>::success();
}

[[nodiscard]] constexpr std::uint64_t align8(const std::uint64_t value) noexcept {
    return (value + 7U) & ~std::uint64_t{7};
}

void append_zeroes(Bytes& bytes, const std::size_t count) {
    bytes.insert(bytes.end(), count, std::byte{0});
}

void align_to_8(Bytes& bytes) {
    append_zeroes(bytes, static_cast<std::size_t>(align8(bytes.size()) - bytes.size()));
}

void write_u8(Bytes& bytes, const std::size_t offset, const std::uint8_t value) {
    bytes[offset] = static_cast<std::byte>(value);
}

void write_u16(Bytes& bytes, const std::size_t offset, const std::uint16_t value) {
    for (std::uint32_t index = 0; index < 2U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

void write_u32(Bytes& bytes, const std::size_t offset, const std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

void write_u64(Bytes& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8U; ++index) {
        bytes[offset + index] = static_cast<std::byte>(value >> (index * 8U));
    }
}

void write_f32(Bytes& bytes, const std::size_t offset, const float value) {
    write_u32(bytes, offset, std::bit_cast<std::uint32_t>(value));
}

void write_f64(Bytes& bytes, const std::size_t offset, const double value) {
    write_u64(bytes, offset, std::bit_cast<std::uint64_t>(value));
}

void write_bytes(Bytes& destination, const std::size_t offset, const ByteView source) {
    std::copy(source.begin(), source.end(), destination.begin() + static_cast<std::ptrdiff_t>(offset));
}

void write_text(Bytes& destination, const std::size_t offset, const std::string_view text) {
    write_bytes(destination, offset, std::as_bytes(std::span{text}));
}

void append_u8(Bytes& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void append_u16(Bytes& bytes, const std::uint16_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 2U);
    write_u16(bytes, offset, value);
}

void append_u32(Bytes& bytes, const std::uint32_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 4U);
    write_u32(bytes, offset, value);
}

void append_u64(Bytes& bytes, const std::uint64_t value) {
    const std::size_t offset = bytes.size();
    bytes.resize(offset + 8U);
    write_u64(bytes, offset, value);
}

void append_f64(Bytes& bytes, const double value) {
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_bytes(Bytes& bytes, const ByteView value) {
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_text(Bytes& bytes, const std::string_view value) {
    append_bytes(bytes, std::as_bytes(std::span{value}));
}

void append_domain(Bytes& bytes, const std::string_view domain) {
    append_text(bytes, domain);
    append_u8(bytes, 0);
}

[[nodiscard]] Result<Sha256Digest> framed_text_hash(
    const std::string_view domain,
    const std::string_view text) {
    Bytes input;
    append_domain(input, domain);
    append_u64(input, text.size());
    append_text(input, text);
    return sha256(input);
}

[[nodiscard]] std::string json_string(const std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size() + 2U);
    encoded.push_back('"');
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    for (const char raw_character : value) {
        const auto character = static_cast<unsigned char>(raw_character);
        switch (character) {
            case '"': encoded += "\\\""; break;
            case '\\': encoded += "\\\\"; break;
            case '\b': encoded += "\\b"; break;
            case '\t': encoded += "\\t"; break;
            case '\n': encoded += "\\n"; break;
            case '\f': encoded += "\\f"; break;
            case '\r': encoded += "\\r"; break;
            default:
                if (character < 0x20U) {
                    encoded += "\\u00";
                    encoded.push_back(hex[character >> 4U]);
                    encoded.push_back(hex[character & 0x0FU]);
                } else {
                    encoded.push_back(static_cast<char>(character));
                }
                break;
        }
    }
    encoded.push_back('"');
    return encoded;
}

[[nodiscard]] bool unsigned_utf8_less(
    const std::string& left,
    const std::string& right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](const char a, const char b) {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
        });
}

[[nodiscard]] Result<DatasetArtifact> make_synthetic_dataset(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity) {
    DatasetArtifact dataset;
    dataset.id = identity.dataset_ids.front();
    dataset.strings = {
        "Synthetic P0 Analytic Terrain",
        "LunarTerrainBuilder",
        "",
        "",
        "1",
        configuration.synthetic_source_uri,
        "LunarQSC_v1",
        "Generated test data",
    };
    dataset.nominal_resolution_meters = synthetic_resolution_meters;
    dataset.effective_resolution_meters = synthetic_resolution_meters;
    dataset.horizontal_accuracy_meters = std::bit_cast<double>(0x7FF8000000000000ULL);
    dataset.vertical_accuracy_meters = std::bit_cast<double>(0x7FF8000000000000ULL);
    dataset.source_no_data = std::bit_cast<double>(0x7FF8000000000000ULL);
    dataset.datum_version = "synthetic_datum_v1";
    dataset.sampling_algorithm = "synthetic_analytic_v1";
    const std::string artifact_name = "synthetic/p0-v1.json";
    const std::string descriptor = fmt::format(
        "{{\"amplitude_meters\":{},\"formula\":\"analytic_unit_vector_v1\",\"version\":1}}",
        configuration.synthetic_amplitude_meters);
    Bytes artifact_bytes{
        std::as_bytes(std::span{descriptor}).begin(),
        std::as_bytes(std::span{descriptor}).end()};
    auto artifact_hash = sha256(artifact_bytes);
    if (!artifact_hash) {
        return Result<DatasetArtifact>::failure(std::move(artifact_hash).error());
    }
    dataset.artifact_members.push_back(ArtifactMember{
        artifact_name,
        artifact_bytes.size(),
        artifact_hash.value(),
    });

    Bytes bundle_input;
    append_domain(bundle_input, "LTDB_ARTIFACT_BUNDLE_V1");
    append_u32(bundle_input, 1);
    append_u32(bundle_input, static_cast<std::uint32_t>(artifact_name.size()));
    append_text(bundle_input, artifact_name);
    append_u64(bundle_input, artifact_bytes.size());
    append_bytes(bundle_input, artifact_hash.value().bytes);
    auto bundle_hash = sha256(bundle_input);
    if (!bundle_hash) {
        return Result<DatasetArtifact>::failure(std::move(bundle_hash).error());
    }
    dataset.artifact_bundle_hash = bundle_hash.value();
    dataset.artifact_bundle_bytes = artifact_bytes.size();

    dataset.metadata_json = fmt::format(
        "{{\"artifact_members\":[{{\"bytes\":{},\"name\":{},\"sha256\":{}}}],"
        "\"datum\":{{\"reference_radius_m\":1737400}},"
        "\"elevation_representation\":\"elevation_meters\",\"metadata_overrides\":{{}},"
        "\"no_data\":\"none\",\"stable_key\":{}}}",
        artifact_bytes.size(),
        json_string(artifact_name),
        json_string(artifact_hash.value().to_hex()),
        json_string(configuration.synthetic_stable_key));

    Bytes registry_input;
    append_domain(registry_input, "LTDB_DATASET_REGISTRY_V1");
    append_u32(registry_input, 1);
    append_u32(registry_input, dataset.id.value);
    append_u32(registry_input, 0);
    for (const std::string& text : dataset.strings) {
        append_u64(registry_input, text.size());
        append_text(registry_input, text);
    }
    append_f64(registry_input, dataset.nominal_resolution_meters);
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, dataset.artifact_bundle_bytes);
    append_bytes(registry_input, dataset.artifact_bundle_hash.bytes);
    append_u64(registry_input, dataset.metadata_json.size());
    append_text(registry_input, dataset.metadata_json);
    append_u32(registry_input, 0);
    auto registry_hash = sha256(registry_input);
    if (!registry_hash) {
        return Result<DatasetArtifact>::failure(std::move(registry_hash).error());
    }
    dataset.registry_hash = registry_hash.value();
    return Result<DatasetArtifact>::success(std::move(dataset));
}

[[nodiscard]] Result<DatabaseId> make_database_id(
    const Sha256Digest& builder_hash,
    const Sha256Digest& registry_hash) {
    Bytes input;
    append_domain(input, "LTDB_DATABASE_ID_V1");
    append_bytes(input, builder_hash.bytes);
    append_bytes(input, registry_hash.bytes);
    auto digest = sha256(input);
    if (!digest) {
        return Result<DatabaseId>::failure(std::move(digest).error());
    }
    DatabaseId id;
    std::copy_n(digest.value().bytes.begin(), id.bytes.size(), id.bytes.begin());
    return Result<DatabaseId>::success(id);
}

[[nodiscard]] std::string database_id_hex(const DatabaseId& id) {
    constexpr std::array<char, 16> hex{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    std::string value;
    value.reserve(id.bytes.size() * 2U);
    for (const std::byte byte : id.bytes) {
        const auto integer = std::to_integer<std::uint8_t>(byte);
        value.push_back(hex[integer >> 4U]);
        value.push_back(hex[integer & 0x0FU]);
    }
    return value;
}

[[nodiscard]] double synthetic_elevation(
    const LunarGeodeticCoordinate coordinate,
    const std::int32_t amplitude_meters) noexcept {
    const double cosine = std::cos(coordinate.latitude_radians);
    const double x = cosine * std::cos(coordinate.longitude_radians);
    const double y = cosine * std::sin(coordinate.longitude_radians);
    const double z = std::sin(coordinate.latitude_radians);
    const double normalized = 0.55 * z + 0.25 * x * y + 0.20 * (x * x - y * y);
    return static_cast<double>(amplitude_meters) * normalized;
}

[[nodiscard]] Bytes u16_samples_to_bytes(const std::vector<std::uint16_t>& samples) {
    Bytes bytes;
    bytes.reserve(samples.size() * 2U);
    for (const std::uint16_t sample : samples) {
        append_u16(bytes, sample);
    }
    return bytes;
}

[[nodiscard]] Bytes delta2d_u16(const std::vector<std::uint16_t>& samples) {
    Bytes residuals;
    residuals.reserve(samples.size() * 2U);
    constexpr std::uint32_t width = format_v1::serialized_elevation_samples;
    for (std::uint32_t y = 0; y < format_v1::serialized_elevation_samples; ++y) {
        for (std::uint32_t x = 0; x < format_v1::serialized_elevation_samples; ++x) {
            const std::size_t index = std::size_t{y} * width + x;
            std::uint16_t predicted = 0;
            if (y == 0 && x > 0) {
                predicted = samples[index - 1U];
            } else if (x == 0 && y > 0) {
                predicted = samples[index - width];
            } else if (x > 0 && y > 0) {
                const std::uint32_t value =
                    std::uint32_t{samples[index - 1U]} +
                    std::uint32_t{samples[index - width]} -
                    std::uint32_t{samples[index - width - 1U]};
                predicted = static_cast<std::uint16_t>(value);
            }
            append_u16(residuals, static_cast<std::uint16_t>(samples[index] - predicted));
        }
    }
    return residuals;
}

[[nodiscard]] Result<Bytes> compress_zstandard(const ByteView logical) {
    std::unique_ptr<ZSTD_CCtx, ZstdContextDeleter> context{ZSTD_createCCtx()};
    if (!context) {
        return failure<Bytes>(ErrorCode::internal_error, "could not allocate Zstandard context");
    }
    const std::array parameters{
        std::pair{ZSTD_c_compressionLevel, 3},
        std::pair{ZSTD_c_contentSizeFlag, 1},
        std::pair{ZSTD_c_checksumFlag, 0},
        std::pair{ZSTD_c_dictIDFlag, 0},
        std::pair{ZSTD_c_nbWorkers, 0},
        std::pair{ZSTD_c_enableLongDistanceMatching, 0},
    };
    for (const auto [parameter, value] : parameters) {
        const std::size_t result = ZSTD_CCtx_setParameter(context.get(), parameter, value);
        if (ZSTD_isError(result) != 0) {
            return failure<Bytes>(
                ErrorCode::internal_error,
                fmt::format("could not configure Zstandard: {}", ZSTD_getErrorName(result)));
        }
    }
    const std::size_t pledged = ZSTD_CCtx_setPledgedSrcSize(context.get(), logical.size());
    if (ZSTD_isError(pledged) != 0) {
        return failure<Bytes>(
            ErrorCode::internal_error,
            fmt::format("could not set Zstandard content size: {}", ZSTD_getErrorName(pledged)));
    }
    Bytes stored(ZSTD_compressBound(logical.size()));
    const std::size_t compressed = ZSTD_compress2(
        context.get(), stored.data(), stored.size(), logical.data(), logical.size());
    if (ZSTD_isError(compressed) != 0) {
        return failure<Bytes>(
            ErrorCode::internal_error,
            fmt::format("Zstandard compression failed: {}", ZSTD_getErrorName(compressed)));
    }
    stored.resize(compressed);
    return Result<Bytes>::success(std::move(stored));
}

[[nodiscard]] Bytes provenance_bytes(const FusionTileSummary& summary) {
    const bool has_map = !summary.dominant_source_indices.empty();
    const std::uint8_t index_width = has_map
        ? static_cast<std::uint8_t>(summary.palette.size() <= 256 ? 1 : 2)
        : 0;
    const std::size_t map_bytes = has_map
        ? summary.dominant_source_indices.size() * index_width
        : 0;
    Bytes bytes(
        format_v1::bytes::provenance_header +
        summary.palette.size() * format_v1::bytes::provenance_palette_entry + map_bytes);
    write_u16(bytes, format_v1::provenance_header_offset::version, 1);
    write_u16(
        bytes,
        format_v1::provenance_header_offset::palette_count,
        static_cast<std::uint16_t>(summary.palette.size()));
    if (has_map) {
        write_u16(bytes, format_v1::provenance_header_offset::map_width, 64);
        write_u16(bytes, format_v1::provenance_header_offset::map_height, 64);
        write_u8(bytes, format_v1::provenance_header_offset::index_width, index_width);
        write_u16(bytes, format_v1::provenance_header_offset::flags, 1);
    }
    double previous_fractions = 0.0;
    for (std::size_t index = 0; index < summary.palette.size(); ++index) {
        const FusionPaletteEntry& entry = summary.palette[index];
        const std::size_t offset = format_v1::bytes::provenance_header +
            index * format_v1::bytes::provenance_palette_entry;
        write_u32(bytes, offset, entry.dataset_id.value);
        const float fraction = index + 1U == summary.palette.size()
            ? static_cast<float>(1.0 - previous_fractions)
            : static_cast<float>(entry.contribution_fraction);
        previous_fractions += static_cast<double>(fraction);
        write_f32(bytes, offset + 8U, fraction);
        write_f32(bytes, offset + 12U, static_cast<float>(entry.native_resolution_meters));
    }
    std::size_t map_offset = format_v1::bytes::provenance_header +
        summary.palette.size() * format_v1::bytes::provenance_palette_entry;
    for (const std::uint16_t index : summary.dominant_source_indices) {
        write_u8(bytes, map_offset, static_cast<std::uint8_t>(index & 0xFFU));
        if (index_width == 2) {
            write_u8(bytes, map_offset + 1U, static_cast<std::uint8_t>(index >> 8U));
        }
        map_offset += index_width;
    }
    return bytes;
}

[[nodiscard]] FusionTileSummary single_source_summary(const DatasetArtifact& dataset) {
    FusionTileSummary summary;
    summary.primary_dataset = dataset.id;
    summary.palette.push_back(FusionPaletteEntry{
        dataset.id, 1.0, dataset.nominal_resolution_meters});
    return summary;
}

[[nodiscard]] Result<Sha256Digest> tile_dependency_hash(
    const LunarTileKey key,
    const ConfigurationIdentity& identity,
    const std::span<const DatasetArtifact> datasets,
    const std::span<const std::optional<Sha256Digest>> window_dependencies,
    const std::string_view fusion_algorithm) {
    if (datasets.empty() || datasets.size() != window_dependencies.size()) {
        return failure<Sha256Digest>(
            ErrorCode::invalid_argument, "tile dependency sources are inconsistent");
    }
    const std::string semantic_hex = identity.semantic_hash.to_hex();
    std::vector<std::size_t> order(datasets.size());
    std::iota(order.begin(), order.end(), 0U);
    std::ranges::sort(order, {}, [&datasets](const std::size_t index) {
        return datasets[index].id.value;
    });
    std::string sources;
    for (const std::size_t index : order) {
        if (!sources.empty()) {
            sources.push_back(',');
        }
        const std::string windows = window_dependencies[index]
            ? fmt::format(
                "[{{\"dependency_sha256\":\"{}\"}}]",
                window_dependencies[index]->to_hex())
            : "[]";
        sources += fmt::format(
            "{{\"artifact_bundle_sha256\":\"{}\",\"dataset_id\":{},\"windows\":{}}}",
            datasets[index].artifact_bundle_hash.to_hex(),
            datasets[index].id.value,
            windows);
    }
    const std::string jcs = fmt::format(
        "{{\"builder_algorithm_version\":1,\"datum_version\":{},"
        "\"fusion\":{{\"algorithm\":{},\"configuration_sha256\":\"{}\"}},"
        "\"projection\":{{\"id\":1,\"implementation_version\":1}},"
        "\"quantization\":{{\"configuration_sha256\":\"{}\",\"id\":1}},"
        "\"semantic_configuration_sha256\":\"{}\",\"sources\":[{}],"
        "\"tile_key\":\"{:016x}\",\"tile_schema_version\":1}}",
        json_string(datasets.front().datum_version),
        json_string(fusion_algorithm),
        semantic_hex,
        semantic_hex,
        semantic_hex,
        sources,
        key.encoded());
    return framed_text_hash("LTDB_TILE_DEP_V1", jcs);
}

[[nodiscard]] Result<Sha256Digest> tile_content_hash(
    const LunarTileKey key,
    const std::vector<ChannelArtifact>& channels) {
    Bytes input;
    append_domain(input, "LTDB_TILE_CONTENT_V1");
    append_u64(input, key.encoded());
    append_u16(input, static_cast<std::uint16_t>(channels.size()));
    for (const ChannelArtifact& channel : channels) {
        append_u16(input, static_cast<std::uint16_t>(channel.id));
        append_u16(input, 1);
        append_u8(input, static_cast<std::uint8_t>(channel.element_type));
        append_u8(input, 1);
        append_u8(input, static_cast<std::uint8_t>(channel.predictor));
        append_u8(input, 0);
        append_u16(input, channel.width);
        append_u16(input, channel.height);
        append_u32(input, channel.flags);
        append_u32(input, static_cast<std::uint32_t>(channel.decoded.size()));
        append_u32(input, 0);
        append_u32(input, channel.parameter1);
        append_bytes(input, channel.decoded);
    }
    return sha256(input);
}

[[nodiscard]] Result<EncodedTile> encode_tile(
    const LunarTileKey key,
    const std::vector<std::uint16_t>& samples,
    const Sha256Digest& dependency_hash,
    const std::span<const DatasetArtifact> datasets,
    const FusionTileSummary& summary,
    const double geometric_error_meters = 0.0,
    const std::uint8_t materialized_child_mask = 0) {
    const auto primary = std::ranges::find_if(datasets, [&summary](const DatasetArtifact& dataset) {
        return dataset.id == summary.primary_dataset;
    });
    if (primary == datasets.end() || summary.palette.empty() ||
        summary.palette.size() > std::numeric_limits<std::uint16_t>::max() ||
        !std::isfinite(geometric_error_meters) || geometric_error_meters < 0.0 ||
        geometric_error_meters * 1'000.0 >
            static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
        (materialized_child_mask & 0xF0U) != 0) {
        return failure<EncodedTile>(
            ErrorCode::invalid_argument, "tile provenance does not resolve a primary dataset");
    }
    ChannelArtifact elevation;
    elevation.id = ChannelId::elevation;
    elevation.element_type = ElementType::u16;
    elevation.predictor = Predictor::delta2d_u16;
    elevation.width = format_v1::serialized_elevation_samples;
    elevation.height = format_v1::serialized_elevation_samples;
    elevation.flags = required_channel;
    elevation.parameter1 = static_cast<std::uint32_t>(QuantizationId::global_u16_0p5m);
    elevation.decoded = u16_samples_to_bytes(samples);
    elevation.logical = delta2d_u16(samples);
    auto elevation_stored = compress_zstandard(elevation.logical);
    if (!elevation_stored) {
        Error error = std::move(elevation_stored).error();
        error.with_tile_key(key.encoded()).with_channel(static_cast<std::uint16_t>(ChannelId::elevation));
        return Result<EncodedTile>::failure(std::move(error));
    }
    elevation.stored = std::move(elevation_stored).value();

    ChannelArtifact provenance;
    provenance.id = ChannelId::provenance;
    provenance.element_type = ElementType::opaque;
    provenance.predictor = Predictor::none;
    provenance.flags = required_channel;
    provenance.width = summary.dominant_source_indices.empty() ? 0 : 64;
    provenance.height = summary.dominant_source_indices.empty() ? 0 : 64;
    provenance.decoded = provenance_bytes(summary);
    provenance.logical = provenance.decoded;
    auto provenance_stored = compress_zstandard(provenance.logical);
    if (!provenance_stored) {
        Error error = std::move(provenance_stored).error();
        error.with_tile_key(key.encoded()).with_channel(static_cast<std::uint16_t>(ChannelId::provenance));
        return Result<EncodedTile>::failure(std::move(error));
    }
    provenance.stored = std::move(provenance_stored).value();

    std::vector<ChannelArtifact> channels;
    channels.push_back(std::move(elevation));
    channels.push_back(std::move(provenance));

    if (!summary.quality.empty()) {
        if (summary.quality.size() != 64U * 64U) {
            return failure<EncodedTile>(
                ErrorCode::invalid_argument, "tile quality map must contain 64x64 samples");
        }
        ChannelArtifact quality;
        quality.id = ChannelId::quality;
        quality.element_type = ElementType::u8;
        quality.predictor = Predictor::none;
        quality.width = 64;
        quality.height = 64;
        quality.decoded.reserve(summary.quality.size());
        for (const std::uint8_t value : summary.quality) {
            quality.decoded.push_back(static_cast<std::byte>(value));
        }
        quality.logical = quality.decoded;
        auto quality_stored = compress_zstandard(quality.logical);
        if (!quality_stored) {
            Error error = std::move(quality_stored).error();
            error.with_tile_key(key.encoded()).with_channel(
                static_cast<std::uint16_t>(ChannelId::quality));
            return Result<EncodedTile>::failure(std::move(error));
        }
        quality.stored = std::move(quality_stored).value();
        channels.push_back(std::move(quality));
    }

    auto content_hash = tile_content_hash(key, channels);
    if (!content_hash) {
        return Result<EncodedTile>::failure(std::move(content_hash).error());
    }

    const auto [minimum, maximum] = std::ranges::minmax_element(samples);
    const std::uint64_t directory_bytes = channels.size() * format_v1::bytes::channel_record;
    const std::uint64_t data_offset = align8(format_v1::bytes::tile_header + directory_bytes);
    Bytes payload(static_cast<std::size_t>(data_offset));
    write_text(payload, format_v1::tile_header_offset::magic, "LTIL");
    write_u16(payload, format_v1::tile_header_offset::version, 1);
    write_u16(payload, format_v1::tile_header_offset::header_bytes, format_v1::bytes::tile_header);
    write_u64(payload, format_v1::tile_header_offset::tile_key, key.encoded());
    const std::uint32_t tile_flags = tile_has_provenance |
        (summary.quality.empty() ? 0U : tile_has_quality);
    write_u32(payload, format_v1::tile_header_offset::flags, tile_flags);
    write_u16(payload, format_v1::tile_header_offset::channel_count, static_cast<std::uint16_t>(channels.size()));
    write_u16(payload, format_v1::tile_header_offset::tile_cells, format_v1::tile_cells);
    write_u16(payload, format_v1::tile_header_offset::core_vertices, format_v1::core_vertices);
    write_u8(payload, format_v1::tile_header_offset::apron, format_v1::apron_samples);
    write_u8(payload, format_v1::tile_header_offset::encoding_profile, static_cast<std::uint8_t>(EncodingProfile::global_u16));
    write_f32(payload, format_v1::tile_header_offset::effective_resolution,
              static_cast<float>(primary->effective_resolution_meters));
    write_f32(
        payload,
        format_v1::tile_header_offset::geometric_error,
        static_cast<float>(geometric_error_meters));
    write_f32(payload, format_v1::tile_header_offset::minimum_elevation, -16'384.0F + static_cast<float>(*minimum) * 0.5F);
    write_f32(payload, format_v1::tile_header_offset::maximum_elevation, -16'384.0F + static_cast<float>(*maximum) * 0.5F);
    write_u32(payload, format_v1::tile_header_offset::primary_dataset, summary.primary_dataset.value);
    write_u16(
        payload,
        format_v1::tile_header_offset::provenance_palette_count,
        static_cast<std::uint16_t>(summary.palette.size()));
    write_u32(payload, format_v1::tile_header_offset::channel_directory_offset, format_v1::bytes::tile_header);
    write_u32(payload, format_v1::tile_header_offset::channel_directory_bytes, static_cast<std::uint32_t>(directory_bytes));
    write_u32(payload, format_v1::tile_header_offset::data_region_offset, static_cast<std::uint32_t>(data_offset));
    write_bytes(payload, format_v1::tile_header_offset::dependency_hash, ByteView{dependency_hash.bytes}.first<16>());
    write_bytes(payload, format_v1::tile_header_offset::content_hash, ByteView{content_hash.value().bytes}.first<16>());

    std::uint64_t logical_sum = 0;
    for (std::size_t index = 0; index < channels.size(); ++index) {
        align_to_8(payload);
        const ChannelArtifact& channel = channels[index];
        if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
            channel.stored.size() > std::numeric_limits<std::uint32_t>::max() ||
            channel.logical.size() > std::numeric_limits<std::uint32_t>::max()) {
            return failure<EncodedTile>(ErrorCode::arithmetic_overflow, "encoded channel exceeds v1 limits");
        }
        const std::uint32_t channel_data_offset = static_cast<std::uint32_t>(payload.size());
        const std::size_t record_offset =
            format_v1::bytes::tile_header + index * format_v1::bytes::channel_record;
        write_u16(payload, record_offset + format_v1::channel_record_offset::channel_id, static_cast<std::uint16_t>(channel.id));
        write_u16(payload, record_offset + format_v1::channel_record_offset::version, 1);
        write_u8(payload, record_offset + format_v1::channel_record_offset::element_type, static_cast<std::uint8_t>(channel.element_type));
        write_u8(payload, record_offset + format_v1::channel_record_offset::components, 1);
        write_u8(payload, record_offset + format_v1::channel_record_offset::codec, static_cast<std::uint8_t>(channel.codec));
        write_u8(payload, record_offset + format_v1::channel_record_offset::predictor, static_cast<std::uint8_t>(channel.predictor));
        write_u16(payload, record_offset + format_v1::channel_record_offset::width, channel.width);
        write_u16(payload, record_offset + format_v1::channel_record_offset::height, channel.height);
        write_u32(payload, record_offset + format_v1::channel_record_offset::flags, channel.flags);
        write_u32(payload, record_offset + format_v1::channel_record_offset::data_offset, channel_data_offset);
        write_u32(payload, record_offset + format_v1::channel_record_offset::stored_bytes, static_cast<std::uint32_t>(channel.stored.size()));
        write_u32(payload, record_offset + format_v1::channel_record_offset::logical_bytes, static_cast<std::uint32_t>(channel.logical.size()));
        write_u32(payload, record_offset + format_v1::channel_record_offset::crc32c, crc32c(channel.stored));
        write_u32(payload, record_offset + format_v1::channel_record_offset::parameter0, 3);
        write_u32(payload, record_offset + format_v1::channel_record_offset::parameter1, channel.parameter1);
        append_bytes(payload, channel.stored);
        logical_sum += channel.logical.size();
    }
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() ||
        logical_sum > std::numeric_limits<std::uint32_t>::max()) {
        return failure<EncodedTile>(ErrorCode::arithmetic_overflow, "encoded tile exceeds v1 limits");
    }
    write_u32(
        payload,
        format_v1::tile_header_offset::data_region_bytes,
        static_cast<std::uint32_t>(payload.size() - data_offset));

    EncodedTile encoded{
        key,
        std::move(payload),
        *minimum,
        *maximum,
        dependency_hash,
        content_hash.value(),
        0,
        static_cast<std::uint32_t>(logical_sum),
        static_cast<std::uint32_t>(std::llround(primary->effective_resolution_meters * 1'000.0)),
        static_cast<std::uint32_t>(std::llround(geometric_error_meters * 1'000.0)),
        tile_flags,
        summary.primary_dataset,
        static_cast<std::uint16_t>(summary.palette.size()),
        materialized_child_mask,
        static_cast<std::uint8_t>(channels.size()),
    };
    encoded.payload_crc = crc32c(encoded.payload);
    encoded.payload_bytes = static_cast<std::uint32_t>(encoded.payload.size());
    return Result<EncodedTile>::success(std::move(encoded));
}

[[nodiscard]] Result<TileBuildResult> build_tiles(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const DatasetArtifact& dataset,
    BuildCache& cache,
    const BuildOptions& options) {
    const ElevationSampler sampler = [&configuration](const QscCoordinate qsc) {
        auto coordinate = QscProjection::Inverse(qsc);
        if (!coordinate) {
            return Result<double>::failure(std::move(coordinate).error());
        }
        return Result<double>::success(synthetic_elevation(
            coordinate.value(), configuration.synthetic_amplitude_meters));
    };

    std::vector<LunarTileKey> keys;
    std::vector<Sha256Digest> dependencies;
    keys.reserve(6);
    dependencies.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto key = LunarTileKey::create(face, 0, 0, 0);
        if (!key) {
            return Result<TileBuildResult>::failure(std::move(key).error());
        }
        const std::array datasets{dataset};
        const std::array<std::optional<Sha256Digest>, 1> windows{std::nullopt};
        auto dependency = tile_dependency_hash(
            key.value(), identity, datasets, windows, dataset.sampling_algorithm);
        if (!dependency) {
            return Result<TileBuildResult>::failure(std::move(dependency).error());
        }
        keys.push_back(key.value());
        dependencies.push_back(dependency.value());
    }

    std::vector<std::optional<StagedElevationTile>> staged_slots(keys.size());
    std::vector<std::optional<CachedTileRecord>> reused_records(keys.size());
    std::vector<std::optional<EncodedTile>> reused_tiles(keys.size());
    std::vector<BuilderTask> tasks;
    tasks.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
        const std::filesystem::path expected_path = staged_elevation_artifact_path(
            configuration.cache_directory / "staging", keys[index], dependencies[index]);
        if (options.incremental) {
            auto reusable = cache.FindReusableTile(keys[index], dependencies[index]);
            if (!reusable) {
                return Result<TileBuildResult>::failure(std::move(reusable).error());
            }
            if (reusable.value()) {
                auto encoded = load_encoded_tile_artifact(
                    reusable.value()->staging_path,
                    keys[index],
                    dependencies[index],
                    reusable.value()->content_hash);
                auto core = load_staged_elevation_tile(
                    staged_elevation_artifact_path(
                        configuration.cache_directory / "staging",
                        keys[index],
                        dependencies[index]),
                    keys[index],
                    dependencies[index]);
                if (encoded && core) {
                    staged_slots[index] = std::move(core).value();
                    reused_tiles[index] = std::move(encoded).value();
                    reused_records[index] = std::move(*reusable.value());
                    continue;
                }
            }
        }
        auto marked = cache.MarkTileBuilding(keys[index], dependencies[index], expected_path);
        if (!marked) {
            return Result<TileBuildResult>::failure(std::move(marked).error());
        }
        tasks.emplace_back([&, index]() -> Result<void> {
            TelemetryActivity sampling_activity{options.execution.telemetry, "sampling"};
            auto staged = stage_elevation_tile(
                keys[index],
                dependencies[index],
                configuration.cache_directory / "staging",
                sampler);
            if (!staged) {
                return Result<void>::failure(std::move(staged).error());
            }
            staged_slots[index] = std::move(staged).value();
            if (options.execution.telemetry != nullptr) {
                options.execution.telemetry->AddCount(
                    "requested_source_samples",
                    std::uint64_t{format_v1::core_vertices} * format_v1::core_vertices);
                record_staging_file(
                    options.execution.telemetry, staged_slots[index]->artifact_path);
            }
            return Result<void>::success();
        });
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWorkers(
            (std::min)(configuration.worker_threads, static_cast<std::uint32_t>(tasks.size())),
            0,
            tasks.size());
    }
    auto executed = run_bounded_tasks(
        tasks, configuration.worker_threads, options.cancellation_token());
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWorkers(0, 0, 0);
    }
    if (!executed) {
        return Result<TileBuildResult>::failure(std::move(executed).error());
    }

    std::vector<StagedElevationTile> staged;
    staged.reserve(staged_slots.size());
    for (std::optional<StagedElevationTile>& slot : staged_slots) {
        if (!slot) {
            return failure<TileBuildResult>(
                ErrorCode::internal_error, "bounded tile task omitted a staged result");
        }
        staged.push_back(std::move(*slot));
    }
    auto resolved = [&]() {
        TelemetryActivity seam_activity{options.execution.telemetry, "seam_resolution"};
        return resolve_elevation_boundaries(staged);
    }();
    if (!resolved) {
        return Result<TileBuildResult>::failure(std::move(resolved).error());
    }
    auto finalized = [&]() {
        TelemetryActivity quantization_activity{options.execution.telemetry, "quantization"};
        return finalize_elevation_tiles(staged, sampler);
    }();
    if (!finalized) {
        return Result<TileBuildResult>::failure(std::move(finalized).error());
    }
    auto hierarchy = compute_hierarchy_metadata(finalized.value());
    if (!hierarchy) {
        return Result<TileBuildResult>::failure(std::move(hierarchy).error());
    }

    TileBuildResult result;
    result.tiles.reserve(finalized.value().size());
    const FusionTileSummary summary = single_source_summary(dataset);
    TelemetryActivity encoding_activity{options.execution.telemetry, "encoding"};
    for (std::size_t index = 0; index < finalized.value().size(); ++index) {
        const FinalizedElevationTile& elevation = finalized.value()[index];
        auto tile = encode_tile(
            elevation.key,
            elevation.serialized_samples,
            elevation.dependency_hash,
            std::span{&dataset, std::size_t{1}},
            summary,
            hierarchy.value()[index].geometric_error_meters,
            hierarchy.value()[index].materialized_child_mask);
        if (!tile) {
            return Result<TileBuildResult>::failure(std::move(tile).error());
        }
        if (reused_records[index]) {
            if (reused_records[index]->content_hash != tile.value().content_hash ||
                reused_tiles[index]->payload != tile.value().payload) {
                return failure<TileBuildResult>(
                    ErrorCode::hash_mismatch,
                    "dependency-identical cached tile rebuilt to different content");
            }
            ++result.reused_tile_count;
        } else {
            const std::filesystem::path encoded_path = encoded_tile_artifact_path(
                configuration, elevation.key, elevation.dependency_hash);
            auto persisted = persist_encoded_tile_artifact(encoded_path, tile.value());
            if (!persisted) {
                return Result<TileBuildResult>::failure(std::move(persisted).error());
            }
            record_staging_file(options.execution.telemetry, encoded_path);
            auto stored = [&]() {
                TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
                return cache.StoreCompletedTile(
                    elevation.key,
                    elevation.dependency_hash,
                    tile.value().content_hash,
                    encoded_path);
            }();
            if (!stored) {
                return Result<TileBuildResult>::failure(std::move(stored).error());
            }
            ++result.built_tile_count;
            if (options.execution.telemetry != nullptr) {
                options.execution.telemetry->AddCount("tiles_encoded");
            }
        }
        result.tiles.push_back(std::move(tile).value());
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetWork(index + 1U, finalized.value().size());
        }
    }
    return Result<TileBuildResult>::success(std::move(result));
}

[[nodiscard]] Result<std::vector<PackArtifact>> build_packs(
    const BuilderConfiguration& configuration,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles,
    const BuildOptions& options) {
    if (tiles.empty() || !std::ranges::is_sorted(tiles, {}, [](const EncodedTile& tile) {
            return tile.key;
        })) {
        return failure<std::vector<PackArtifact>>(
            ErrorCode::invalid_argument, "canonical packing requires nonempty sorted tiles");
    }
    std::vector<PackArtifact> packs;
    packs.reserve(tiles.size());
    const std::string id_hex = database_id_hex(database_id);
    std::vector<PackingTile> packing_tiles;
    packing_tiles.reserve(tiles.size());
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        if ((index % 256U) == 0U) {
            auto checked = check_build_execution(options);
            if (!checked) {
                return Result<std::vector<PackArtifact>>::failure(
                    std::move(checked).error());
            }
        }
        const EncodedTile& tile = tiles[index];
        const std::size_t payload_bytes = tile.payload.empty()
            ? tile.payload_bytes
            : tile.payload.size();
        if (payload_bytes == 0) {
            return failure<std::vector<PackArtifact>>(
                ErrorCode::invalid_argument,
                "encoded tile has no packable payload",
                tile.artifact_path);
        }
        packing_tiles.push_back(PackingTile{tile.key, payload_bytes});
    }
    auto ranges = plan_canonical_pack_ranges(
        packing_tiles, configuration.target_pack_bytes, options.cancellation_token());
    if (!ranges) {
        return Result<std::vector<PackArtifact>>::failure(std::move(ranges).error());
    }
    const bool stage_packs_to_disk = std::ranges::any_of(
        tiles, [](const EncodedTile& tile) { return tile.payload.empty(); });
    for (const CanonicalPackRange range : ranges.value()) {
        auto checked = check_build_execution(options);
        if (!checked) {
            return Result<std::vector<PackArtifact>>::failure(std::move(checked).error());
        }
        const std::uint32_t pack_number = static_cast<std::uint32_t>(packs.size());
        const std::uint8_t face = tiles[range.first_tile].key.face();
        const std::uint8_t level = tiles[range.first_tile].key.level();
        Bytes bytes(format_v1::bytes::pack_header);
        std::vector<PackTilePlacement> placements;
        placements.reserve(range.tile_count);
        for (std::size_t tile_index = range.first_tile;
             tile_index < range.first_tile + range.tile_count;
             ++tile_index) {
            if ((tile_index % 64U) == 0U) {
                checked = check_build_execution(options);
                if (!checked) {
                    return Result<std::vector<PackArtifact>>::failure(
                        std::move(checked).error());
                }
                if (options.execution.telemetry != nullptr) {
                    options.execution.telemetry->SetWork(tile_index, tiles.size());
                }
            }
            const EncodedTile& tile = tiles[tile_index];
            std::optional<EncodedTile> loaded;
            if (tile.payload.empty()) {
                auto artifact = load_encoded_tile_artifact(
                    tile.artifact_path,
                    tile.key,
                    tile.dependency_hash,
                    tile.content_hash);
                if (!artifact) {
                    return Result<std::vector<PackArtifact>>::failure(
                        std::move(artifact).error());
                }
                loaded = std::move(artifact).value();
            }
            const Bytes& payload = loaded ? loaded->payload : tile.payload;
            const std::uint64_t payload_offset = align8(bytes.size());
            append_zeroes(
                bytes, static_cast<std::size_t>(payload_offset - bytes.size()));
            append_bytes(bytes, payload);
            placements.push_back(PackTilePlacement{
                tile_index,
                payload_offset,
                static_cast<std::uint32_t>(payload.size()),
            });
        }
        const LunarTileKey first_key = tiles[placements.front().tile_index].key;
        const LunarTileKey last_key = tiles[placements.back().tile_index].key;
        write_text(bytes, format_v1::pack_header_offset::magic, "LTPK");
        write_u16(bytes, format_v1::pack_header_offset::major, format_v1::major_version);
        write_u16(bytes, format_v1::pack_header_offset::minor, format_v1::minor_version);
        write_u32(bytes, format_v1::pack_header_offset::header_bytes, format_v1::bytes::pack_header);
        write_u32(bytes, format_v1::pack_header_offset::endian, format_v1::endian_tag);
        write_u32(bytes, format_v1::pack_header_offset::pack_id, pack_number);
        write_u64(bytes, format_v1::pack_header_offset::tile_count, placements.size());
        write_u64(bytes, format_v1::pack_header_offset::payload_region_offset, format_v1::bytes::pack_header);
        write_u64(bytes, format_v1::pack_header_offset::file_bytes, bytes.size());
        auto pack_hash = sha256(bytes);
        if (!pack_hash) {
            return Result<std::vector<PackArtifact>>::failure(std::move(pack_hash).error());
        }
        write_bytes(bytes, format_v1::pack_header_offset::sha256_prefix, ByteView{pack_hash.value().bytes}.first<16>());
        const std::filesystem::path relative_path = fmt::format(
            "Packs/{}_{}_F{}_L{:02}_P{:04}.ltp",
            configuration.database_name,
            id_hex,
            face,
            level,
            pack_number);
        const std::uint64_t file_bytes = bytes.size();
        if (stage_packs_to_disk) {
            const std::filesystem::path final_path =
                configuration.output_directory / relative_path;
            std::error_code filesystem_error;
            std::filesystem::create_directories(final_path.parent_path(), filesystem_error);
            if (filesystem_error) {
                return Result<std::vector<PackArtifact>>::failure(build_error(
                    ErrorCode::io_error,
                    fmt::format(
                        "could not create streaming pack directory: {}",
                        filesystem_error.message()),
                    final_path.parent_path()));
            }
            const std::filesystem::path temporary = temporary_sibling(final_path);
            auto written = write_file_synced(
                temporary, bytes, options.cancellation_token());
            if (!written) {
                return Result<std::vector<PackArtifact>>::failure(
                    std::move(written).error());
            }
            bytes.clear();
            bytes.shrink_to_fit();
        }
        packs.push_back(PackArtifact{
            PackId{pack_number},
            relative_path,
            std::move(bytes),
            pack_hash.value(),
            first_key,
            last_key,
            std::move(placements),
            file_bytes,
        });
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWork(tiles.size(), tiles.size());
    }
    return Result<std::vector<PackArtifact>>::success(std::move(packs));
}

struct StringTable {
    Bytes bytes;
    std::map<std::string, std::uint32_t, decltype(&unsigned_utf8_less)> offsets{&unsigned_utf8_less};
};

[[nodiscard]] Result<StringTable> make_string_table(
    const std::span<const DatasetArtifact> datasets,
    const std::vector<PackArtifact>& packs) {
    std::vector<std::string> values;
    for (const DatasetArtifact& dataset : datasets) {
        for (const std::string& value : dataset.strings) {
            if (!value.empty()) {
                values.push_back(value);
            }
        }
    }
    for (const PackArtifact& pack : packs) {
        values.push_back(pack.relative_path.generic_string());
    }
    std::ranges::sort(values, unsigned_utf8_less);
    values.erase(std::unique(values.begin(), values.end()), values.end());

    StringTable table;
    append_u32(table.bytes, 0);
    table.offsets.emplace("", 0);
    for (const std::string& value : values) {
        if (table.bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            return failure<StringTable>(ErrorCode::arithmetic_overflow, "STRS exceeds v1 StringID range");
        }
        const std::uint32_t offset = static_cast<std::uint32_t>(table.bytes.size());
        table.offsets.emplace(value, offset);
        append_u32(table.bytes, static_cast<std::uint32_t>(value.size()));
        append_text(table.bytes, value);
        append_zeroes(table.bytes, (4U - (table.bytes.size() % 4U)) % 4U);
    }
    return Result<StringTable>::success(std::move(table));
}

[[nodiscard]] Bytes make_dataset_chunk(
    const std::span<const DatasetArtifact> datasets,
    const StringTable& strings) {
    Bytes bytes(datasets.size() * format_v1::bytes::dataset_record);
    std::uint32_t meta_offset = 0;
    for (std::size_t dataset_index = 0; dataset_index < datasets.size(); ++dataset_index) {
        const DatasetArtifact& dataset = datasets[dataset_index];
        const std::size_t offset = dataset_index * format_v1::bytes::dataset_record;
        write_u32(bytes, offset + format_v1::dataset_record_offset::dataset_id, dataset.id.value);
        for (std::size_t index = 0; index < dataset.strings.size(); ++index) {
            write_u32(
                bytes,
                offset + format_v1::dataset_record_offset::first_string_id + index * 4U,
                strings.offsets.at(dataset.strings[index]));
        }
        write_u32(bytes, offset + format_v1::dataset_record_offset::flags, dataset.flags);
        write_f64(bytes, offset + format_v1::dataset_record_offset::nominal_resolution, dataset.nominal_resolution_meters);
        write_f64(bytes, offset + format_v1::dataset_record_offset::horizontal_accuracy, dataset.horizontal_accuracy_meters);
        write_f64(bytes, offset + format_v1::dataset_record_offset::vertical_accuracy, dataset.vertical_accuracy_meters);
        write_f64(bytes, offset + format_v1::dataset_record_offset::source_no_data, dataset.source_no_data);
        write_u64(bytes, offset + format_v1::dataset_record_offset::artifact_bundle_bytes, dataset.artifact_bundle_bytes);
        write_bytes(bytes, offset + format_v1::dataset_record_offset::artifact_bundle_hash, dataset.artifact_bundle_hash.bytes);
        write_u32(bytes, offset + format_v1::dataset_record_offset::meta_offset, meta_offset);
        write_u32(bytes, offset + format_v1::dataset_record_offset::meta_bytes, static_cast<std::uint32_t>(dataset.metadata_json.size()));
        meta_offset += static_cast<std::uint32_t>(dataset.metadata_json.size());
    }
    return bytes;
}

[[nodiscard]] Bytes make_meta_chunk(const std::span<const DatasetArtifact> datasets) {
    Bytes bytes;
    for (const DatasetArtifact& dataset : datasets) {
        append_text(bytes, dataset.metadata_json);
    }
    return bytes;
}

[[nodiscard]] Result<Sha256Digest> dataset_registry_hash(
    const std::span<const DatasetArtifact> datasets) {
    Bytes input;
    append_domain(input, "LTDB_DATASET_REGISTRY_V1");
    append_u32(input, static_cast<std::uint32_t>(datasets.size()));
    for (const DatasetArtifact& dataset : datasets) {
        append_u32(input, dataset.id.value);
        append_u32(input, dataset.flags);
        for (const std::string& text : dataset.strings) {
            append_u64(input, text.size());
            append_text(input, text);
        }
        append_f64(input, dataset.nominal_resolution_meters);
        append_f64(input, dataset.horizontal_accuracy_meters);
        append_f64(input, dataset.vertical_accuracy_meters);
        append_f64(input, dataset.source_no_data);
        append_u64(input, dataset.artifact_bundle_bytes);
        append_bytes(input, dataset.artifact_bundle_hash.bytes);
        append_u64(input, dataset.metadata_json.size());
        append_text(input, dataset.metadata_json);
        append_u32(input, 0);
    }
    return sha256(input);
}

[[nodiscard]] Bytes make_pack_chunk(
    const std::vector<PackArtifact>& packs,
    const StringTable& strings) {
    Bytes bytes(packs.size() * format_v1::bytes::pack_record);
    for (std::size_t index = 0; index < packs.size(); ++index) {
        const PackArtifact& pack = packs[index];
        const std::size_t offset = index * format_v1::bytes::pack_record;
        write_u32(bytes, offset + format_v1::pack_record_offset::pack_id, pack.id.value);
        write_u32(bytes, offset + format_v1::pack_record_offset::path_string_id,
                  strings.offsets.at(pack.relative_path.generic_string()));
        write_u16(bytes, offset + format_v1::pack_record_offset::default_codec, static_cast<std::uint16_t>(Codec::zstandard));
        write_u64(
            bytes,
            offset + format_v1::pack_record_offset::tile_count,
            pack.placements.size());
        write_u64(bytes, offset + format_v1::pack_record_offset::file_bytes, pack.file_bytes);
        write_u64(bytes, offset + format_v1::pack_record_offset::first_tile_key, pack.first_key.encoded());
        write_u64(bytes, offset + format_v1::pack_record_offset::last_tile_key, pack.last_key.encoded());
        write_bytes(bytes, offset + format_v1::pack_record_offset::sha256, pack.hash.bytes);
    }
    return bytes;
}

[[nodiscard]] Result<Bytes> make_tile_index_chunk(
    const std::vector<PackArtifact>& packs,
    const std::vector<EncodedTile>& tiles,
    const BuildOptions& options) {
    Bytes bytes(tiles.size() * format_v1::bytes::tile_index_record);
    for (const PackArtifact& pack : packs) {
        for (const PackTilePlacement& placement : pack.placements) {
            if ((placement.tile_index % 256U) == 0U) {
                auto checked = check_build_execution(options);
                if (!checked) {
                    return Result<Bytes>::failure(std::move(checked).error());
                }
            }
            const EncodedTile& tile = tiles[placement.tile_index];
            const std::size_t offset =
                placement.tile_index * format_v1::bytes::tile_index_record;
            write_u64(bytes, offset + format_v1::tile_index_offset::tile_key, tile.key.encoded());
            write_u32(bytes, offset + format_v1::tile_index_offset::pack_id, pack.id.value);
            write_u32(bytes, offset + format_v1::tile_index_offset::flags, tile.flags);
            write_u64(
                bytes,
                offset + format_v1::tile_index_offset::payload_offset,
                placement.payload_offset);
            write_u32(
                bytes,
                offset + format_v1::tile_index_offset::stored_bytes,
                placement.payload_bytes);
            write_u32(bytes, offset + format_v1::tile_index_offset::logical_bytes, tile.logical_channel_bytes);
            write_u16(bytes, offset + format_v1::tile_index_offset::minimum_elevation, tile.minimum_code);
            write_u16(bytes, offset + format_v1::tile_index_offset::maximum_elevation, tile.maximum_code);
            write_u32(
                bytes,
                offset + format_v1::tile_index_offset::primary_dataset,
                tile.primary_dataset.value);
            write_u32(bytes, offset + format_v1::tile_index_offset::effective_resolution,
                      tile.effective_resolution_millimeters);
            write_u32(bytes, offset + format_v1::tile_index_offset::geometric_error,
                      tile.geometric_error_millimeters);
            write_u8(bytes, offset + format_v1::tile_index_offset::child_mask,
                     tile.materialized_child_mask);
            write_u8(bytes, offset + format_v1::tile_index_offset::channel_count, tile.channel_count);
            write_u32(bytes, offset + format_v1::tile_index_offset::payload_crc32c, tile.payload_crc);
            write_bytes(bytes, offset + format_v1::tile_index_offset::content_hash, ByteView{tile.content_hash.bytes}.first<16>());
            write_bytes(bytes, offset + format_v1::tile_index_offset::dependency_hash, ByteView{tile.dependency_hash.bytes}.first<8>());
        }
    }
    return Result<Bytes>::success(std::move(bytes));
}

[[nodiscard]] Result<Sha256Digest> database_content_hash(
    const ConfigurationIdentity& identity,
    const std::vector<ChunkArtifact>& chunks,
    const std::vector<PackArtifact>& packs,
    const BuildOptions& options) {
    Bytes input;
    append_domain(input, "LTDB_DATABASE_CONTENT_V1");
    append_u16(input, format_v1::major_version);
    append_u16(input, format_v1::minor_version);
    append_bytes(input, identity.builder_hash.bytes);
    append_u32(input, static_cast<std::uint32_t>(chunks.size()));
    for (const ChunkArtifact& chunk : chunks) {
        auto checked = check_build_execution(options);
        if (!checked) {
            return Result<Sha256Digest>::failure(std::move(checked).error());
        }
        append_text(input, std::string_view{chunk.tag.data(), chunk.tag.size()});
        append_u16(input, 1);
        append_u16(input, mandatory_chunk_flags);
        append_u64(input, chunk.bytes.size());
        append_bytes(input, chunk.bytes);
    }
    append_u32(input, static_cast<std::uint32_t>(packs.size()));
    for (std::size_t index = 0; index < packs.size(); ++index) {
        if ((index % 256U) == 0U) {
            auto checked = check_build_execution(options);
            if (!checked) {
                return Result<Sha256Digest>::failure(std::move(checked).error());
            }
        }
        const PackArtifact& pack = packs[index];
        append_u32(input, pack.id.value);
        append_bytes(input, pack.hash.bytes);
    }
    return sha256(input);
}

[[nodiscard]] Result<std::pair<Bytes, Sha256Digest>> make_database_file(
    const ConfigurationIdentity& identity,
    const std::span<const DatasetArtifact> datasets,
    const Sha256Digest& registry_hash,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles,
    const std::vector<PackArtifact>& packs,
    const BuildOptions& options) {
    auto checked = check_build_execution(options);
    if (!checked) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(
            std::move(checked).error());
    }
    auto strings = make_string_table(datasets, packs);
    if (!strings) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(strings).error());
    }
    auto tidx = make_tile_index_chunk(packs, tiles, options);
    if (!tidx) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(
            std::move(tidx).error());
    }
    auto tidx_hash = framed_text_hash("LTDB_TILE_INDEX_V1", std::string_view{
        reinterpret_cast<const char*>(tidx.value().data()), tidx.value().size()});
    if (!tidx_hash) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(tidx_hash).error());
    }

    std::vector<ChunkArtifact> chunks{
        ChunkArtifact{{'D', 'S', 'E', 'T'}, make_dataset_chunk(datasets, strings.value()), 0},
        ChunkArtifact{{'M', 'E', 'T', 'A'}, make_meta_chunk(datasets), 0},
        ChunkArtifact{{'P', 'A', 'C', 'K'}, make_pack_chunk(packs, strings.value()), 0},
        ChunkArtifact{{'S', 'T', 'R', 'S'}, std::move(strings).value().bytes, 0},
        ChunkArtifact{{'T', 'I', 'D', 'X'}, std::move(tidx).value(), 0},
    };
    auto content_hash = database_content_hash(identity, chunks, packs, options);
    if (!content_hash) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(content_hash).error());
    }

    const std::uint64_t directory_bytes = chunks.size() * format_v1::bytes::chunk_directory_entry;
    Bytes file(static_cast<std::size_t>(align8(format_v1::bytes::ltdb_header + directory_bytes)));
    for (ChunkArtifact& chunk : chunks) {
        checked = check_build_execution(options);
        if (!checked) {
            return Result<std::pair<Bytes, Sha256Digest>>::failure(
                std::move(checked).error());
        }
        align_to_8(file);
        chunk.file_offset = file.size();
        append_bytes(file, chunk.bytes);
    }

    write_text(file, format_v1::ltdb_header_offset::magic, "LTDB");
    write_u16(file, format_v1::ltdb_header_offset::major, format_v1::major_version);
    write_u16(file, format_v1::ltdb_header_offset::minor, format_v1::minor_version);
    write_u32(file, format_v1::ltdb_header_offset::header_bytes, format_v1::bytes::ltdb_header);
    write_u32(file, format_v1::ltdb_header_offset::endian, format_v1::endian_tag);
    write_u32(file, format_v1::ltdb_header_offset::chunk_count, static_cast<std::uint32_t>(chunks.size()));
    write_u64(file, format_v1::ltdb_header_offset::chunk_directory, format_v1::bytes::ltdb_header);
    write_bytes(file, format_v1::ltdb_header_offset::database_id, database_id.bytes);
    write_f64(file, format_v1::ltdb_header_offset::reference_radius, 1'737'400.0);
    write_f64(file, format_v1::ltdb_header_offset::elevation_origin, -16'384.0);
    write_f64(file, format_v1::ltdb_header_offset::elevation_step, 0.5);
    write_u16(file, format_v1::ltdb_header_offset::tile_cells, format_v1::tile_cells);
    write_u16(file, format_v1::ltdb_header_offset::core_vertices, format_v1::core_vertices);
    write_u8(file, format_v1::ltdb_header_offset::apron, format_v1::apron_samples);
    const auto highest_level = std::ranges::max_element(
        tiles, {}, [](const EncodedTile& tile) { return tile.key.level(); });
    const std::uint8_t maximum_level = highest_level->key.level();
    write_u8(file, format_v1::ltdb_header_offset::maximum_level, maximum_level);
    write_u8(file, format_v1::ltdb_header_offset::projection, static_cast<std::uint8_t>(ProjectionId::lunar_qsc_v1));
    write_u8(file, format_v1::ltdb_header_offset::quantization, static_cast<std::uint8_t>(QuantizationId::global_u16_0p5m));
    write_u64(file, format_v1::ltdb_header_offset::tile_count, tiles.size());
    write_u32(file, format_v1::ltdb_header_offset::dataset_count, static_cast<std::uint32_t>(datasets.size()));
    write_u32(file, format_v1::ltdb_header_offset::pack_count, static_cast<std::uint32_t>(packs.size()));
    write_bytes(file, format_v1::ltdb_header_offset::builder_configuration_hash, identity.builder_hash.bytes);
    write_bytes(file, format_v1::ltdb_header_offset::dataset_registry_hash, registry_hash.bytes);
    write_bytes(file, format_v1::ltdb_header_offset::tile_index_hash, tidx_hash.value().bytes);
    write_bytes(file, format_v1::ltdb_header_offset::database_content_hash, content_hash.value().bytes);

    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const ChunkArtifact& chunk = chunks[index];
        const std::size_t offset = format_v1::bytes::ltdb_header + index * format_v1::bytes::chunk_directory_entry;
        write_text(file, offset + format_v1::chunk_directory_offset::tag,
                   std::string_view{chunk.tag.data(), chunk.tag.size()});
        write_u16(file, offset + format_v1::chunk_directory_offset::version, 1);
        write_u16(file, offset + format_v1::chunk_directory_offset::flags, mandatory_chunk_flags);
        write_u64(file, offset + format_v1::chunk_directory_offset::file_offset, chunk.file_offset);
        write_u64(file, offset + format_v1::chunk_directory_offset::stored_bytes, chunk.bytes.size());
        write_u64(file, offset + format_v1::chunk_directory_offset::logical_bytes, chunk.bytes.size());
        write_u32(file, offset + format_v1::chunk_directory_offset::crc32c, crc32c(chunk.bytes));
    }
    return Result<std::pair<Bytes, Sha256Digest>>::success(
        std::pair{std::move(file), content_hash.value()});
}

[[nodiscard]] Result<void> write_file_synced(
    const std::filesystem::path& path,
    const ByteView bytes,
    const std::stop_token cancellation) {
    if (cancellation.stop_requested()) {
        return Result<void>::failure(
            build_error(ErrorCode::cancelled, "output write was cancelled", path));
    }
    constexpr std::size_t maximum_write_bytes = 8U * 1024U * 1024U;
#ifdef _WIN32
    HANDLE handle = CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not create temporary output (Windows error {})", GetLastError()),
            path));
    }
    std::size_t offset = 0;
    bool succeeded = true;
    bool cancelled = false;
    while (offset < bytes.size()) {
        if (cancellation.stop_requested()) {
            cancelled = true;
            break;
        }
        const std::size_t remaining = bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, maximum_write_bytes));
        DWORD written = 0;
        if (WriteFile(handle, bytes.data() + offset, requested, &written, nullptr) == FALSE ||
            written != requested) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && !cancelled && cancellation.stop_requested()) {
        cancelled = true;
    }
    if (succeeded && !cancelled && FlushFileBuffers(handle) == FALSE) {
        succeeded = false;
    }
    if (CloseHandle(handle) == FALSE) {
        succeeded = false;
    }
    if (!succeeded) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not durably write temporary output (Windows error {})", GetLastError()),
            path));
    }
    if (cancelled) {
        return Result<void>::failure(
            build_error(ErrorCode::cancelled, "output write was cancelled", path));
    }
#else
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (descriptor < 0) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not create temporary output: {}", std::strerror(errno)),
            path));
    }
    std::size_t offset = 0;
    bool succeeded = true;
    bool cancelled = false;
    while (offset < bytes.size()) {
        if (cancellation.stop_requested()) {
            cancelled = true;
            break;
        }
        const std::size_t requested =
            (std::min)(bytes.size() - offset, maximum_write_bytes);
        const ssize_t written = ::write(descriptor, bytes.data() + offset, requested);
        if (written <= 0) {
            succeeded = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (succeeded && !cancelled && cancellation.stop_requested()) {
        cancelled = true;
    }
    if (succeeded && !cancelled && ::fsync(descriptor) != 0) {
        succeeded = false;
    }
    if (::close(descriptor) != 0) {
        succeeded = false;
    }
    if (!succeeded) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not durably write temporary output: {}", std::strerror(errno)),
            path));
    }
    if (cancelled) {
        return Result<void>::failure(
            build_error(ErrorCode::cancelled, "output write was cancelled", path));
    }
#endif
    return Result<void>::success();
}

[[nodiscard]] Result<Bytes> read_file(
    const std::filesystem::path& path,
    const std::stop_token cancellation = {}) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return failure<Bytes>(ErrorCode::io_error, "could not open existing output", path);
    }
    const std::streamoff size = stream.tellg();
    if (size < 0 || static_cast<std::uint64_t>(size) > std::numeric_limits<std::size_t>::max()) {
        return failure<Bytes>(ErrorCode::arithmetic_overflow, "existing output is too large", path);
    }
    Bytes bytes(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    constexpr std::size_t maximum_read_bytes = 8U * 1024U * 1024U;
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancellation.stop_requested()) {
            return failure<Bytes>(ErrorCode::cancelled, "file read was cancelled", path);
        }
        const std::size_t requested =
            (std::min)(bytes.size() - offset, maximum_read_bytes);
        stream.read(
            reinterpret_cast<char*>(bytes.data() + offset),
            static_cast<std::streamsize>(requested));
        if (!stream) {
            return failure<Bytes>(ErrorCode::io_error, "could not read existing output", path);
        }
        offset += requested;
    }
    return Result<Bytes>::success(std::move(bytes));
}

[[nodiscard]] std::uint16_t read_le_u16(
    const ByteView bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_le_u32(
    const ByteView bytes,
    const std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_le_u64(
    const ByteView bytes,
    const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8U; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

std::filesystem::path encoded_tile_artifact_path(
    const BuilderConfiguration& configuration,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash) {
    return configuration.cache_directory / "staging" / fmt::format(
        "{:016x}-{}.tile-v2", key.encoded(), dependency_hash.to_hex());
}

Result<void> persist_encoded_tile_artifact(
    const std::filesystem::path& path,
    const EncodedTile& tile) {
    constexpr std::size_t header_bytes = 120;
    Bytes bytes(header_bytes);
    write_text(bytes, 0, "LTET");
    write_u16(bytes, 4, 2);
    write_u64(bytes, 8, tile.key.encoded());
    write_bytes(bytes, 16, tile.dependency_hash.bytes);
    write_bytes(bytes, 48, tile.content_hash.bytes);
    write_u16(bytes, 80, tile.minimum_code);
    write_u16(bytes, 82, tile.maximum_code);
    write_u32(bytes, 84, tile.payload_crc);
    write_u32(bytes, 88, tile.logical_channel_bytes);
    write_u32(bytes, 92, tile.effective_resolution_millimeters);
    write_u32(bytes, 96, tile.geometric_error_millimeters);
    write_u32(bytes, 100, tile.flags);
    write_u32(bytes, 104, tile.primary_dataset.value);
    write_u16(bytes, 108, tile.provenance_palette_count);
    write_u8(bytes, 110, tile.materialized_child_mask);
    write_u8(bytes, 111, tile.channel_count);
    write_u32(bytes, 112, static_cast<std::uint32_t>(tile.payload.size()));
    append_bytes(bytes, tile.payload);
    write_u32(bytes, 116, crc32c(bytes));

    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not create encoded-tile staging directory: {}",
                        filesystem_error.message()),
            path.parent_path()));
    }
    const bool exists = std::filesystem::exists(path, filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not inspect encoded-tile staging artifact: {}",
                        filesystem_error.message()),
            path).with_tile_key(tile.key.encoded()));
    }
    if (exists) {
        auto existing = read_file(path);
        if (!existing) {
            return Result<void>::failure(std::move(existing).error());
        }
        if (existing.value() == bytes) {
            return Result<void>::success();
        }
        return Result<void>::failure(build_error(
            ErrorCode::hash_mismatch,
            "dependency-identical encoded tile staging artifact has different bytes",
            path).with_tile_key(tile.key.encoded()));
    }
    const std::filesystem::path temporary =
        path.parent_path() / ("." + path.filename().string() + ".tmp");
    auto written = write_file_synced(temporary, bytes);
    if (!written) {
        return written;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        const std::string rename_message = filesystem_error.message();
        std::error_code remove_error;
        std::filesystem::remove(temporary, remove_error);
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not publish encoded-tile staging artifact: {}",
                        rename_message),
            path).with_tile_key(tile.key.encoded()));
    }
    return Result<void>::success();
}

Result<EncodedTile> load_encoded_tile_artifact(
    const std::filesystem::path& path,
    const LunarTileKey expected_key,
    const Sha256Digest& expected_dependency_hash,
    const Sha256Digest& expected_content_hash) {
    constexpr std::size_t header_bytes = 120;
    auto file = read_file(path);
    if (!file) {
        return Result<EncodedTile>::failure(std::move(file).error());
    }
    const ByteView bytes = file.value();
    if (bytes.size() < header_bytes ||
        std::string_view{reinterpret_cast<const char*>(bytes.data()), 4} != "LTET" ||
        read_le_u16(bytes, 4) != 2 || read_le_u16(bytes, 6) != 0 ||
        read_le_u64(bytes, 8) != expected_key.encoded() ||
        !std::ranges::equal(
            bytes.subspan(16, expected_dependency_hash.bytes.size()),
            expected_dependency_hash.bytes) ||
        !std::ranges::equal(
            bytes.subspan(48, expected_content_hash.bytes.size()),
            expected_content_hash.bytes)) {
        return failure<EncodedTile>(
            ErrorCode::hash_mismatch,
            "encoded tile staging identity does not match the requested cache row",
            path);
    }
    const std::uint32_t artifact_crc = read_le_u32(bytes, 116);
    Bytes checksum_bytes = file.value();
    write_u32(checksum_bytes, 116, 0);
    if (crc32c(checksum_bytes) != artifact_crc) {
        return failure<EncodedTile>(
            ErrorCode::checksum_mismatch,
            "encoded tile staging artifact CRC does not match",
            path);
    }
    const std::uint32_t payload_bytes = read_le_u32(bytes, 112);
    if (payload_bytes < format_v1::bytes::tile_header ||
        bytes.size() != header_bytes + std::size_t{payload_bytes}) {
        return failure<EncodedTile>(
            ErrorCode::invalid_format, "encoded tile staging payload size is invalid", path);
    }
    Bytes payload(bytes.begin() + static_cast<std::ptrdiff_t>(header_bytes), bytes.end());
    const std::uint32_t payload_crc = read_le_u32(bytes, 84);
    const ByteView payload_view = payload;
    if (crc32c(payload) != payload_crc ||
        std::string_view{reinterpret_cast<const char*>(payload.data()), 4} != "LTIL" ||
        read_le_u64(payload_view, format_v1::tile_header_offset::tile_key) !=
            expected_key.encoded() ||
        !std::ranges::equal(
            payload_view.subspan(
                format_v1::tile_header_offset::dependency_hash, 16),
            ByteView{expected_dependency_hash.bytes}.first<16>()) ||
        !std::ranges::equal(
            payload_view.subspan(format_v1::tile_header_offset::content_hash, 16),
            ByteView{expected_content_hash.bytes}.first<16>())) {
        return failure<EncodedTile>(
            ErrorCode::checksum_mismatch,
            "encoded tile staging payload failed identity or CRC validation",
            path);
    }
    return Result<EncodedTile>::success(EncodedTile{
        expected_key,
        std::move(payload),
        read_le_u16(bytes, 80),
        read_le_u16(bytes, 82),
        expected_dependency_hash,
        expected_content_hash,
        payload_crc,
        read_le_u32(bytes, 88),
        read_le_u32(bytes, 92),
        read_le_u32(bytes, 96),
        read_le_u32(bytes, 100),
        DatasetId{read_le_u32(bytes, 104)},
        read_le_u16(bytes, 108),
        static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(bytes[110])),
        static_cast<std::uint8_t>(std::to_integer<std::uint8_t>(bytes[111])),
        path,
        payload_bytes,
    });
}

void record_staging_file(
    TelemetryCollector* const collector,
    const std::filesystem::path& path) {
    if (collector == nullptr) {
        return;
    }
    std::error_code filesystem_error;
    const std::uint64_t bytes = std::filesystem::file_size(path, filesystem_error);
    if (!filesystem_error) {
        collector->AddStagingBytes(bytes);
        collector->AddStagingLiveBytes(bytes);
    }
}

[[nodiscard]] std::filesystem::path temporary_sibling(const std::filesystem::path& final_path) {
    return final_path.parent_path() / ("." + final_path.filename().string() + ".tmp");
}

void remove_if_present(const std::filesystem::path& path) noexcept {
    std::error_code error;
    std::filesystem::remove(path, error);
}

[[nodiscard]] Result<void> atomic_replace(
    const std::filesystem::path& temporary,
    const std::filesystem::path& final_path) {
#ifdef _WIN32
    if (MoveFileExW(
            temporary.c_str(), final_path.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not atomically publish output (Windows error {})", GetLastError()),
            final_path));
    }
#else
    if (::rename(temporary.c_str(), final_path.c_str()) != 0) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not atomically publish output: {}", std::strerror(errno)),
            final_path));
    }
#endif
    return Result<void>::success();
}

[[nodiscard]] Result<void> publish_outputs(
    const std::filesystem::path& database_path,
    const Bytes& database_bytes,
    const std::vector<PackArtifact>& packs,
    const BuildOptions& options) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(database_path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format("could not create output directory: {}", filesystem_error.message()),
            database_path.parent_path()));
    }

    std::vector<std::filesystem::path> temporary_paths;
    temporary_paths.reserve(packs.size() + 1U);
    const auto cleanup = [&temporary_paths]() noexcept {
        for (const auto& path : temporary_paths) {
            remove_if_present(path);
        }
    };

    for (std::size_t index = 0; index < packs.size(); ++index) {
        auto checked = check_build_execution(options);
        if (!checked) {
            cleanup();
            return checked;
        }
        const PackArtifact& pack = packs[index];
        const std::filesystem::path final_path = database_path.parent_path() / pack.relative_path;
        std::filesystem::create_directories(final_path.parent_path(), filesystem_error);
        if (filesystem_error) {
            cleanup();
            return Result<void>::failure(build_error(
                ErrorCode::io_error,
                fmt::format("could not create pack directory: {}", filesystem_error.message()),
                final_path.parent_path()));
        }
        const std::filesystem::path temporary = temporary_sibling(final_path);
        temporary_paths.push_back(temporary);
        if (pack.bytes.empty()) {
            const std::uintmax_t temporary_bytes =
                std::filesystem::file_size(temporary, filesystem_error);
            if (filesystem_error || temporary_bytes != pack.file_bytes) {
                const std::string message = filesystem_error
                    ? filesystem_error.message()
                    : "staged pack size does not match its record";
                cleanup();
                return Result<void>::failure(build_error(
                    ErrorCode::io_error,
                    fmt::format("could not inspect staged pack: {}", message),
                    temporary));
            }
        } else {
            auto written = write_file_synced(
                temporary, pack.bytes, options.cancellation_token());
            if (!written) {
                cleanup();
                return written;
            }
        }
    }
    const std::filesystem::path database_temporary = temporary_sibling(database_path);
    temporary_paths.push_back(database_temporary);
    auto database_written = write_file_synced(
        database_temporary, database_bytes, options.cancellation_token());
    if (!database_written) {
        cleanup();
        return database_written;
    }

    for (const PackArtifact& pack : packs) {
        auto checked = check_build_execution(options);
        if (!checked) {
            cleanup();
            return checked;
        }
        const std::filesystem::path final_path = database_path.parent_path() / pack.relative_path;
        if (!std::filesystem::exists(final_path, filesystem_error)) {
            if (filesystem_error) {
                cleanup();
                return Result<void>::failure(build_error(
                    ErrorCode::io_error,
                    fmt::format("could not inspect existing pack: {}", filesystem_error.message()),
                    final_path));
            }
            continue;
        }
        auto existing = read_file(final_path, options.cancellation_token());
        if (!existing) {
            cleanup();
            return Result<void>::failure(std::move(existing).error());
        }
        bool matches = false;
        if (!pack.bytes.empty()) {
            matches = existing.value() == pack.bytes;
        } else if (existing.value().size() == pack.file_bytes &&
                   existing.value().size() >= format_v1::bytes::pack_header) {
            std::fill_n(
                existing.value().begin() +
                    static_cast<std::ptrdiff_t>(format_v1::pack_header_offset::sha256_prefix),
                16U,
                std::byte{0});
            auto existing_hash = sha256(existing.value());
            if (!existing_hash) {
                cleanup();
                return Result<void>::failure(std::move(existing_hash).error());
            }
            matches = existing_hash.value() == pack.hash;
        }
        if (!matches) {
            cleanup();
            return Result<void>::failure(build_error(
                ErrorCode::hash_mismatch,
                "an existing content-addressed pack has different bytes",
                final_path));
        }
    }

    for (std::size_t index = 0; index < packs.size(); ++index) {
        auto checked = check_build_execution(options);
        if (!checked) {
            cleanup();
            return checked;
        }
        const PackArtifact& pack = packs[index];
        const std::filesystem::path final_path = database_path.parent_path() / pack.relative_path;
        const std::filesystem::path temporary = temporary_sibling(final_path);
        if (std::filesystem::exists(final_path, filesystem_error)) {
            remove_if_present(temporary);
            continue;
        }
        std::filesystem::rename(temporary, final_path, filesystem_error);
        if (filesystem_error) {
            cleanup();
            return Result<void>::failure(build_error(
                ErrorCode::io_error,
                fmt::format("could not publish pack: {}", filesystem_error.message()),
                final_path));
        }
    }

    auto checked = check_build_execution(options);
    if (!checked) {
        cleanup();
        return checked;
    }
    if (std::filesystem::exists(database_path, filesystem_error)) {
        auto existing = read_file(database_path, options.cancellation_token());
        if (!existing) {
            cleanup();
            return Result<void>::failure(std::move(existing).error());
        }
        if (existing.value() == database_bytes) {
            remove_if_present(database_temporary);
            return Result<void>::success();
        }
    }
    auto published = atomic_replace(database_temporary, database_path);
    if (!published) {
        cleanup();
        return published;
    }
    return Result<void>::success();
}

[[nodiscard]] Result<TileBuildResult> build_raster_prototype_tiles(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const BuildOptions& options) {
    if (sources.empty() || sources.size() != configuration.rasters.size() ||
        sources.size() != datasets.size()) {
        return failure<TileBuildResult>(
            ErrorCode::invalid_argument, "raster fusion sources are inconsistent");
    }
    std::vector<std::size_t> source_order(sources.size());
    std::iota(source_order.begin(), source_order.end(), 0U);
    std::ranges::sort(source_order, {}, [&configuration, &datasets](const std::size_t index) {
        return std::tuple{configuration.rasters[index].priority, datasets[index].id.value};
    });
    const std::size_t target_source = source_order.back();
    auto target_level = choose_source_level(
        sources[target_source]->details().footprint,
        datasets[target_source].effective_resolution_meters,
        configuration.maximum_level);
    if (!target_level) {
        return Result<TileBuildResult>::failure(std::move(target_level).error());
    }
    auto key = choose_raster_prototype_tile(
        *sources[target_source], target_level.value());
    if (!key) {
        return Result<TileBuildResult>::failure(std::move(key).error());
    }

    std::vector<std::optional<Sha256Digest>> window_dependencies;
    window_dependencies.reserve(sources.size());
    for (const IRasterSource* source : sources) {
        auto window = source->WindowDependency(key.value());
        if (!window) {
            return Result<TileBuildResult>::failure(std::move(window).error());
        }
        window_dependencies.push_back(window.value());
    }
    auto dependency = tile_dependency_hash(
        key.value(), identity, datasets, window_dependencies, "heterogeneous_fusion_v1");
    if (!dependency) {
        return Result<TileBuildResult>::failure(std::move(dependency).error());
    }

    const std::filesystem::path encoded_path = encoded_tile_artifact_path(
        configuration, key.value(), dependency.value());
    if (options.incremental) {
        auto reusable = cache.FindReusableTile(key.value(), dependency.value());
        if (!reusable) {
            return Result<TileBuildResult>::failure(std::move(reusable).error());
        }
        if (reusable.value()) {
            auto encoded = load_encoded_tile_artifact(
                reusable.value()->staging_path,
                key.value(),
                dependency.value(),
                reusable.value()->content_hash);
            if (encoded) {
                TileBuildResult result;
                result.tiles.push_back(std::move(encoded).value());
                result.reused_tile_count = 1;
                return Result<TileBuildResult>::success(std::move(result));
            }
        }
    }
    auto marked = cache.MarkTileBuilding(key.value(), dependency.value(), encoded_path);
    if (!marked) {
        return Result<TileBuildResult>::failure(std::move(marked).error());
    }
    if (options.cancellation_token().stop_requested()) {
        return Result<TileBuildResult>::failure(Error{
            ErrorCode::cancelled, "terrain build was cancelled"}
            .with_tile_key(key.value().encoded()));
    }

    double coarsest_resolution = 0.0;
    double finest_resolution = std::numeric_limits<double>::infinity();
    for (const DatasetArtifact& dataset : datasets) {
        coarsest_resolution = std::max(coarsest_resolution, dataset.effective_resolution_meters);
        finest_resolution = std::min(finest_resolution, dataset.effective_resolution_meters);
    }
    const std::uint32_t maximum_passes = residual_filter_pass_count(
        coarsest_resolution, finest_resolution);
    const std::uint32_t halo = 33U + 2U * maximum_passes;
    const std::uint32_t grid_width = format_v1::core_vertices + 2U * halo;
    const std::uint32_t grid_height = grid_width;
    const std::uint64_t cells_per_axis =
        std::uint64_t{format_v1::tile_cells} * (std::uint64_t{1} << key.value().level());
    const std::int64_t first_x =
        static_cast<std::int64_t>(key.value().x()) * format_v1::tile_cells - halo;
    const std::int64_t first_y =
        static_cast<std::int64_t>(key.value().y()) * format_v1::tile_cells - halo;
    if (first_x < 0 || first_y < 0 ||
        first_x + grid_width - 1 > static_cast<std::int64_t>(cells_per_axis) ||
        first_y + grid_height - 1 > static_cast<std::int64_t>(cells_per_axis)) {
        return failure<TileBuildResult>(
            ErrorCode::unsupported_feature,
            "M5 prototype fusion halo crosses a cube-face boundary");
    }

    std::vector<FusionGridSource> fusion_sources;
    fusion_sources.reserve(sources.size());
    {
        TelemetryActivity sampling_activity{options.execution.telemetry, "sampling"};
        for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
        if (options.cancellation_token().stop_requested()) {
            return Result<TileBuildResult>::failure(Error{
                ErrorCode::cancelled, "terrain build was cancelled"}
                .with_tile_key(key.value().encoded()));
        }
        FusionGridSource fusion_source;
        fusion_source.dataset_id = datasets[source_index].id;
        fusion_source.priority = configuration.rasters[source_index].priority;
        fusion_source.policy = configuration.rasters[source_index].fusion_policy;
        fusion_source.native_resolution_meters = datasets[source_index].nominal_resolution_meters;
        fusion_source.width = grid_width;
        fusion_source.height = grid_height;
        fusion_source.samples.resize(std::size_t{grid_width} * grid_height);
        fusion_source.quality.assign(fusion_source.samples.size(), 0);
        for (std::uint32_t y = 0; y < grid_height; ++y) {
            if (options.cancellation_token().stop_requested()) {
                return Result<TileBuildResult>::failure(Error{
                    ErrorCode::cancelled, "terrain build was cancelled"}
                    .with_tile_key(key.value().encoded()));
            }
            for (std::uint32_t x = 0; x < grid_width; ++x) {
                const double u = (
                    2.0 * static_cast<double>(first_x + x) -
                    static_cast<double>(cells_per_axis)) /
                    static_cast<double>(cells_per_axis);
                const double v = (
                    2.0 * static_cast<double>(first_y + y) -
                    static_cast<double>(cells_per_axis)) /
                    static_cast<double>(cells_per_axis);
                auto coordinate = QscProjection::Inverse(QscCoordinate{
                    static_cast<QscFace>(key.value().face()), u, v, 0.0});
                if (!coordinate) {
                    return Result<TileBuildResult>::failure(
                        std::move(coordinate).error());
                }
                auto sample = sources[source_index]->TrySample(coordinate.value());
                if (!sample) {
                    return Result<TileBuildResult>::failure(std::move(sample).error());
                }
                if (!sample.value()) {
                    continue;
                }
                const std::size_t index = std::size_t{y} * grid_width + x;
                fusion_source.samples[index] = sample.value()->elevation_meters;
                fusion_source.quality[index] |= sample.value()->quality_flags;
                if (sample.value()->interpolated) {
                    fusion_source.quality[index] |= quality_interpolated;
                }
                if (sample.value()->filled_no_data) {
                    fusion_source.quality[index] |= quality_filled_no_data;
                }
            }
        }
        fusion_sources.push_back(std::move(fusion_source));
        if (options.execution.telemetry != nullptr) {
            const std::uint64_t sample_count = std::uint64_t{grid_width} * grid_height;
            const std::uint64_t core_count =
                std::uint64_t{format_v1::core_vertices} * format_v1::core_vertices;
            options.execution.telemetry->AddCount("requested_source_samples", sample_count);
            options.execution.telemetry->AddCount(
                "requested_halo_samples", sample_count - core_count);
        }
    }
    }
    auto fused = [&]() {
        TelemetryActivity fusion_activity{options.execution.telemetry, "fusion"};
        return fuse_source_grids(fusion_sources);
    }();
    if (!fused) {
        Error error = std::move(fused).error();
        error.with_tile_key(key.value().encoded());
        return Result<TileBuildResult>::failure(std::move(error));
    }
    auto summary = summarize_fused_core(fused.value(), halo, halo);
    if (!summary) {
        return Result<TileBuildResult>::failure(std::move(summary).error());
    }
    std::vector<double> core_samples;
    core_samples.reserve(
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices);
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        const std::size_t offset = std::size_t{y + halo} * grid_width + halo;
        core_samples.insert(
            core_samples.end(),
            fused.value().elevations.begin() + static_cast<std::ptrdiff_t>(offset),
            fused.value().elevations.begin() + static_cast<std::ptrdiff_t>(
                offset + format_v1::core_vertices));
    }
    const ElevationSampler sampler = [&, face = key.value().face()](const QscCoordinate qsc) {
        if (static_cast<std::uint8_t>(qsc.face) != face) {
            return Result<double>::failure(Error{
                ErrorCode::unsupported_feature,
                "M5 prototype virtual apron crossed a cube-face boundary"});
        }
        const auto grid_x = static_cast<std::int64_t>(std::llround(
            (qsc.u + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - first_x;
        const auto grid_y = static_cast<std::int64_t>(std::llround(
            (qsc.v + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - first_y;
        if (grid_x < 0 || grid_y < 0 ||
            grid_x >= static_cast<std::int64_t>(grid_width) ||
            grid_y >= static_cast<std::int64_t>(grid_height)) {
            return Result<double>::failure(Error{
                ErrorCode::invalid_argument, "fusion apron sample is outside the staged halo"});
        }
        return Result<double>::success(fused.value().elevations[
            static_cast<std::size_t>(grid_y) * grid_width +
            static_cast<std::size_t>(grid_x)]);
    };
    auto staged = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return stage_elevation_tile_samples(
            key.value(),
            dependency.value(),
            configuration.cache_directory / "staging",
            core_samples);
    }();
    if (!staged) {
        return Result<TileBuildResult>::failure(std::move(staged).error());
    }
    record_staging_file(options.execution.telemetry, staged.value().artifact_path);
    std::vector<StagedElevationTile> staged_tiles;
    staged_tiles.push_back(std::move(staged).value());
    auto resolved = [&]() {
        TelemetryActivity seam_activity{options.execution.telemetry, "seam_resolution"};
        return resolve_elevation_boundaries(staged_tiles);
    }();
    if (!resolved) {
        return Result<TileBuildResult>::failure(std::move(resolved).error());
    }
    auto finalized = [&]() {
        TelemetryActivity quantization_activity{options.execution.telemetry, "quantization"};
        return finalize_elevation_tiles(staged_tiles, sampler);
    }();
    if (!finalized) {
        return Result<TileBuildResult>::failure(std::move(finalized).error());
    }
    auto tile = [&]() {
        TelemetryActivity encoding_activity{options.execution.telemetry, "encoding"};
        return encode_tile(
            key.value(),
            finalized.value().front().serialized_samples,
            dependency.value(),
            datasets,
            summary.value());
    }();
    if (!tile) {
        return Result<TileBuildResult>::failure(std::move(tile).error());
    }
    auto persisted = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return persist_encoded_tile_artifact(encoded_path, tile.value());
    }();
    if (!persisted) {
        return Result<TileBuildResult>::failure(std::move(persisted).error());
    }
    record_staging_file(options.execution.telemetry, encoded_path);
    auto stored = [&]() {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        return cache.StoreCompletedTile(
            key.value(), dependency.value(), tile.value().content_hash, encoded_path);
    }();
    if (!stored) {
        return Result<TileBuildResult>::failure(std::move(stored).error());
    }
    TileBuildResult result;
    result.tiles.push_back(std::move(tile).value());
    result.built_tile_count = 1;
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->AddCount("tiles_quantized");
        options.execution.telemetry->AddCount("tiles_encoded");
        options.execution.telemetry->SetWork(1, 1);
    }
    return Result<TileBuildResult>::success(std::move(result));
}

[[nodiscard]] Result<QscCoordinate> map_extended_raster_coordinate(
    QscCoordinate coordinate) {
    for (std::uint8_t transition = 0; transition < 4U; ++transition) {
        std::optional<QscEdge> crossed;
        double depth = 0.0;
        if (coordinate.u < -1.0) {
            crossed = QscEdge::west;
            depth = -1.0 - coordinate.u;
        } else if (coordinate.u > 1.0) {
            crossed = QscEdge::east;
            depth = coordinate.u - 1.0;
        } else if (coordinate.v < -1.0) {
            crossed = QscEdge::south;
            depth = -1.0 - coordinate.v;
        } else if (coordinate.v > 1.0) {
            crossed = QscEdge::north;
            depth = coordinate.v - 1.0;
        } else {
            return Result<QscCoordinate>::success(coordinate);
        }

        const bool vertical = *crossed == QscEdge::west || *crossed == QscEdge::east;
        double parameter = vertical ? coordinate.v : coordinate.u;
        const QscEdgeConnection connection = qsc_edge_connection(coordinate.face, *crossed);
        if (connection.reversed) {
            parameter = -parameter;
        }
        coordinate.face = connection.face;
        switch (connection.edge) {
            case QscEdge::west:
                coordinate.u = -1.0 + depth;
                coordinate.v = parameter;
                break;
            case QscEdge::east:
                coordinate.u = 1.0 - depth;
                coordinate.v = parameter;
                break;
            case QscEdge::south:
                coordinate.u = parameter;
                coordinate.v = -1.0 + depth;
                break;
            case QscEdge::north:
                coordinate.u = parameter;
                coordinate.v = 1.0 - depth;
                break;
        }
    }
    return failure<QscCoordinate>(
        ErrorCode::internal_error,
        "could not map an extended raster-fusion coordinate through QSC topology");
}

[[nodiscard]] GeographicFootprint clipped_footprint(
    GeographicFootprint footprint,
    const std::optional<GeographicBounds>& region) noexcept {
    if (!region) {
        return footprint;
    }
    footprint.west_longitude_degrees = std::max(
        footprint.west_longitude_degrees, region->west_longitude_degrees);
    footprint.east_longitude_degrees = std::min(
        footprint.east_longitude_degrees, region->east_longitude_degrees);
    footprint.south_latitude_degrees = std::max(
        footprint.south_latitude_degrees, region->south_latitude_degrees);
    footprint.north_latitude_degrees = std::min(
        footprint.north_latitude_degrees, region->north_latitude_degrees);
    return footprint;
}

[[nodiscard]] Result<SparseHierarchyPlan> raster_hierarchy_plan(
    const BuilderConfiguration& configuration,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets) {
    std::vector<HierarchySource> hierarchy_sources;
    hierarchy_sources.reserve(sources.size());
    for (std::size_t index = 0; index < sources.size(); ++index) {
        const GeographicFootprint footprint = clipped_footprint(
            sources[index]->details().footprint, configuration.required_region);
        if (footprint.west_longitude_degrees >= footprint.east_longitude_degrees ||
            footprint.south_latitude_degrees >= footprint.north_latitude_degrees) {
            continue;
        }
        hierarchy_sources.push_back(HierarchySource{
            datasets[index].id,
            footprint,
            datasets[index].effective_resolution_meters,
            configuration.maximum_level,
        });
    }
    if (hierarchy_sources.empty()) {
        return failure<SparseHierarchyPlan>(
            ErrorCode::invalid_argument,
            "required region does not intersect any configured raster source");
    }
    return plan_sparse_hierarchy(hierarchy_sources);
}

[[nodiscard]] Result<bool> source_intersects_tile_neighborhood(
    const LunarTileKey key,
    const GeographicFootprint& footprint) {
    auto intersects = tile_intersects_source_footprint(key, footprint);
    if (!intersects || intersects.value()) {
        return intersects;
    }
    constexpr std::array edges{
        QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    for (const QscEdge edge : edges) {
        auto neighbor = qsc_tile_neighbor(key, edge);
        if (!neighbor) {
            return Result<bool>::failure(std::move(neighbor).error());
        }
        intersects = tile_intersects_source_footprint(neighbor.value().key, footprint);
        if (!intersects || intersects.value()) {
            return intersects;
        }
    }
    return Result<bool>::success(false);
}

[[nodiscard]] Result<std::vector<std::size_t>> active_sources_for_tile(
    const LunarTileKey key,
    const std::span<const IRasterSource* const> sources) {
    std::vector<std::size_t> active_sources;
    for (std::size_t index = 0; index < sources.size(); ++index) {
        auto intersects = source_intersects_tile_neighborhood(
            key, sources[index]->details().footprint);
        if (!intersects) {
            return Result<std::vector<std::size_t>>::failure(
                std::move(intersects).error());
        }
        if (intersects.value()) {
            active_sources.push_back(index);
        }
    }
    if (active_sources.empty()) {
        return Result<std::vector<std::size_t>>::failure(Error{
            ErrorCode::invalid_argument,
            "planned raster tile has no intersecting source"}.with_tile_key(key.encoded()));
    }
    return Result<std::vector<std::size_t>>::success(std::move(active_sources));
}

[[nodiscard]] Result<double> sample_fused_point(
    const QscCoordinate qsc,
    const BuilderConfiguration& configuration,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    const std::span<const std::size_t> active_sources) {
    auto coordinate = QscProjection::Inverse(qsc);
    if (!coordinate) {
        return Result<double>::failure(std::move(coordinate).error());
    }
    std::vector<FusionGridSource> fusion_sources;
    fusion_sources.reserve(active_sources.size());
    for (const std::size_t source_index : active_sources) {
        auto sample = sources[source_index]->TrySample(coordinate.value());
        if (!sample) {
            return Result<double>::failure(std::move(sample).error());
        }
        FusionGridSource source;
        source.dataset_id = datasets[source_index].id;
        source.priority = configuration.rasters[source_index].priority;
        source.policy = configuration.rasters[source_index].fusion_policy;
        source.native_resolution_meters = datasets[source_index].nominal_resolution_meters;
        source.width = 1;
        source.height = 1;
        source.samples.resize(1);
        source.quality.assign(1, 0);
        if (sample.value()) {
            source.samples.front() = sample.value()->elevation_meters;
            source.quality.front() |= sample.value()->quality_flags;
            if (sample.value()->interpolated) {
                source.quality.front() |= quality_interpolated;
            }
            if (sample.value()->filled_no_data) {
                source.quality.front() |= quality_filled_no_data;
            }
        }
        fusion_sources.push_back(std::move(source));
    }
    auto fused = fuse_source_grids(fusion_sources);
    if (!fused) {
        return Result<double>::failure(std::move(fused).error());
    }
    return Result<double>::success(fused.value().elevations.front());
}

[[nodiscard]] std::filesystem::path raster_auxiliary_artifact_path(
    const BuilderConfiguration& configuration,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash) {
    return configuration.cache_directory / "staging" / fmt::format(
        "{:016x}-{}.raster-v1", key.encoded(), dependency_hash.to_hex());
}

[[nodiscard]] Result<void> persist_raster_auxiliary_artifact(
    const std::filesystem::path& path,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const RasterTileAuxiliary& auxiliary) {
    constexpr std::size_t header_bytes = 72U;
    constexpr std::size_t expected_apron_samples =
        4U * format_v1::core_vertices + 4U;
    if (auxiliary.summary.palette.empty() ||
        auxiliary.summary.palette.size() > std::numeric_limits<std::uint32_t>::max() ||
        (!auxiliary.summary.dominant_source_indices.empty() &&
         auxiliary.summary.dominant_source_indices.size() != 64U * 64U) ||
        (!auxiliary.summary.quality.empty() &&
         auxiliary.summary.quality.size() != 64U * 64U) ||
        auxiliary.virtual_apron_samples.size() != expected_apron_samples ||
        std::ranges::any_of(auxiliary.virtual_apron_samples, [](const double value) {
            return !std::isfinite(value);
        })) {
        return failure<void>(
            ErrorCode::invalid_argument,
            "raster tile auxiliary data is inconsistent",
            path);
    }
    Bytes bytes(header_bytes);
    write_text(bytes, 0, "LTRA");
    write_u16(bytes, 4, 1);
    write_u64(bytes, 8, key.encoded());
    write_bytes(bytes, 16, dependency_hash.bytes);
    write_u32(bytes, 48, auxiliary.summary.primary_dataset.value);
    write_u32(bytes, 52, static_cast<std::uint32_t>(auxiliary.summary.palette.size()));
    write_u32(
        bytes,
        56,
        static_cast<std::uint32_t>(auxiliary.summary.dominant_source_indices.size()));
    write_u32(bytes, 60, static_cast<std::uint32_t>(auxiliary.summary.quality.size()));
    write_u32(bytes, 64, static_cast<std::uint32_t>(auxiliary.virtual_apron_samples.size()));
    for (const FusionPaletteEntry& entry : auxiliary.summary.palette) {
        append_u32(bytes, entry.dataset_id.value);
        append_f64(bytes, entry.contribution_fraction);
        append_f64(bytes, entry.native_resolution_meters);
    }
    for (const std::uint16_t index : auxiliary.summary.dominant_source_indices) {
        append_u16(bytes, index);
    }
    for (const std::uint8_t value : auxiliary.summary.quality) {
        append_u8(bytes, value);
    }
    for (const double value : auxiliary.virtual_apron_samples) {
        append_f64(bytes, value);
    }
    write_u32(bytes, 68, crc32c(bytes));

    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format(
                "could not create raster auxiliary staging directory: {}",
                filesystem_error.message()),
            path.parent_path()));
    }
    if (std::filesystem::exists(path, filesystem_error)) {
        if (filesystem_error) {
            return Result<void>::failure(build_error(
                ErrorCode::io_error,
                fmt::format(
                    "could not inspect raster auxiliary staging artifact: {}",
                    filesystem_error.message()),
                path));
        }
        auto existing = read_file(path);
        if (!existing) {
            return Result<void>::failure(std::move(existing).error());
        }
        if (existing.value() == bytes) {
            return Result<void>::success();
        }
        return failure<void>(
            ErrorCode::hash_mismatch,
            "dependency-identical raster auxiliary data has different bytes",
            path);
    }
    const std::filesystem::path temporary = temporary_sibling(path);
    auto written = write_file_synced(temporary, bytes);
    if (!written) {
        return written;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        remove_if_present(temporary);
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format(
                "could not publish raster auxiliary staging artifact: {}",
                filesystem_error.message()),
            path));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<RasterTileAuxiliary> load_raster_auxiliary_artifact(
    const std::filesystem::path& path,
    const LunarTileKey expected_key,
    const Sha256Digest& expected_dependency_hash) {
    constexpr std::size_t header_bytes = 72U;
    constexpr std::size_t expected_apron_samples =
        4U * format_v1::core_vertices + 4U;
    auto file = read_file(path);
    if (!file) {
        return Result<RasterTileAuxiliary>::failure(std::move(file).error());
    }
    const ByteView bytes = file.value();
    if (bytes.size() < header_bytes ||
        std::string_view{reinterpret_cast<const char*>(bytes.data()), 4} != "LTRA" ||
        read_le_u16(bytes, 4) != 1 || read_le_u16(bytes, 6) != 0 ||
        read_le_u64(bytes, 8) != expected_key.encoded() ||
        !std::ranges::equal(
            bytes.subspan(16, expected_dependency_hash.bytes.size()),
            expected_dependency_hash.bytes)) {
        return failure<RasterTileAuxiliary>(
            ErrorCode::hash_mismatch,
            "raster auxiliary staging identity does not match",
            path);
    }
    Bytes checksum_bytes = file.value();
    const std::uint32_t stored_crc = read_le_u32(bytes, 68);
    write_u32(checksum_bytes, 68, 0);
    if (crc32c(checksum_bytes) != stored_crc) {
        return failure<RasterTileAuxiliary>(
            ErrorCode::checksum_mismatch,
            "raster auxiliary staging artifact CRC does not match",
            path);
    }
    const std::uint32_t palette_count = read_le_u32(bytes, 52);
    const std::uint32_t dominant_count = read_le_u32(bytes, 56);
    const std::uint32_t quality_count = read_le_u32(bytes, 60);
    const std::uint32_t apron_count = read_le_u32(bytes, 64);
    std::uint64_t expected_bytes = header_bytes;
    expected_bytes += std::uint64_t{palette_count} * 20U;
    expected_bytes += std::uint64_t{dominant_count} * 2U;
    expected_bytes += quality_count;
    expected_bytes += std::uint64_t{apron_count} * 8U;
    if (palette_count == 0 ||
        (dominant_count != 0 && dominant_count != 64U * 64U) ||
        (quality_count != 0 && quality_count != 64U * 64U) ||
        apron_count != expected_apron_samples || expected_bytes != bytes.size()) {
        return failure<RasterTileAuxiliary>(
            ErrorCode::invalid_format,
            "raster auxiliary staging payload size is invalid",
            path);
    }
    RasterTileAuxiliary auxiliary;
    auxiliary.summary.primary_dataset = DatasetId{read_le_u32(bytes, 48)};
    auxiliary.summary.palette.reserve(palette_count);
    std::size_t offset = header_bytes;
    std::uint32_t previous_dataset_id = 0;
    bool primary_present = false;
    for (std::uint32_t index = 0; index < palette_count; ++index) {
        const std::uint32_t dataset_id = read_le_u32(bytes, offset);
        const double fraction = std::bit_cast<double>(read_le_u64(bytes, offset + 4U));
        const double resolution = std::bit_cast<double>(read_le_u64(bytes, offset + 12U));
        if (dataset_id == 0 || (index != 0 && dataset_id <= previous_dataset_id) ||
            !std::isfinite(fraction) || fraction <= 0.0 || fraction > 1.0 ||
            !std::isfinite(resolution) || resolution <= 0.0) {
            return failure<RasterTileAuxiliary>(
                ErrorCode::invalid_format,
                "raster auxiliary palette is not canonical",
                path);
        }
        auxiliary.summary.palette.push_back(FusionPaletteEntry{
            DatasetId{dataset_id}, fraction, resolution});
        previous_dataset_id = dataset_id;
        primary_present = primary_present ||
            dataset_id == auxiliary.summary.primary_dataset.value;
        offset += 20U;
    }
    if (!primary_present) {
        return failure<RasterTileAuxiliary>(
            ErrorCode::invalid_format,
            "raster auxiliary palette does not contain its primary dataset",
            path);
    }
    auxiliary.summary.dominant_source_indices.reserve(dominant_count);
    for (std::uint32_t index = 0; index < dominant_count; ++index) {
        const std::uint16_t source_index = read_le_u16(bytes, offset);
        if (source_index >= palette_count) {
            return failure<RasterTileAuxiliary>(
                ErrorCode::invalid_format,
                "raster auxiliary dominant-source index is out of range",
                path);
        }
        auxiliary.summary.dominant_source_indices.push_back(source_index);
        offset += 2U;
    }
    auxiliary.summary.quality.reserve(quality_count);
    for (std::uint32_t index = 0; index < quality_count; ++index) {
        const std::uint8_t quality = std::to_integer<std::uint8_t>(bytes[offset++]);
        if ((quality & 0xE0U) != 0) {
            return failure<RasterTileAuxiliary>(
                ErrorCode::invalid_format,
                "raster auxiliary quality contains reserved bits",
                path);
        }
        auxiliary.summary.quality.push_back(quality);
    }
    auxiliary.virtual_apron_samples.reserve(apron_count);
    for (std::uint32_t index = 0; index < apron_count; ++index) {
        const double value = std::bit_cast<double>(read_le_u64(bytes, offset));
        if (!std::isfinite(value)) {
            return failure<RasterTileAuxiliary>(
                ErrorCode::invalid_format,
                "raster auxiliary apron contains a non-finite value",
                path);
        }
        auxiliary.virtual_apron_samples.push_back(value);
        offset += 8U;
    }
    return Result<RasterTileAuxiliary>::success(std::move(auxiliary));
}

[[nodiscard]] std::filesystem::path quantized_core_artifact_path(
    const BuilderConfiguration& configuration,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash) {
    return configuration.cache_directory / "staging" / fmt::format(
        "{:016x}-{}.qcore-v1", key.encoded(), dependency_hash.to_hex());
}

[[nodiscard]] Result<void> persist_quantized_core_artifact(
    const std::filesystem::path& path,
    const LunarTileKey key,
    const Sha256Digest& dependency_hash,
    const std::span<const std::uint16_t> samples) {
    constexpr std::size_t header_bytes = 56U;
    constexpr std::size_t expected_samples =
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
    if (samples.size() != expected_samples) {
        return failure<void>(
            ErrorCode::invalid_argument,
            "quantized core must contain 257x257 samples",
            path);
    }
    Bytes bytes(header_bytes);
    write_text(bytes, 0, "LTQC");
    write_u16(bytes, 4, 1);
    write_u64(bytes, 8, key.encoded());
    write_bytes(bytes, 16, dependency_hash.bytes);
    write_u32(bytes, 48, static_cast<std::uint32_t>(samples.size()));
    for (const std::uint16_t sample : samples) {
        append_u16(bytes, sample);
    }
    write_u32(bytes, 52, crc32c(bytes));

    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format(
                "could not create quantized-core staging directory: {}",
                filesystem_error.message()),
            path.parent_path()));
    }
    if (std::filesystem::exists(path, filesystem_error)) {
        if (filesystem_error) {
            return Result<void>::failure(build_error(
                ErrorCode::io_error,
                fmt::format(
                    "could not inspect quantized-core staging artifact: {}",
                    filesystem_error.message()),
                path));
        }
        auto existing = read_file(path);
        if (!existing) {
            return Result<void>::failure(std::move(existing).error());
        }
        if (existing.value() == bytes) {
            return Result<void>::success();
        }
        return failure<void>(
            ErrorCode::hash_mismatch,
            "dependency-identical quantized core has different bytes",
            path);
    }
    const std::filesystem::path temporary = temporary_sibling(path);
    auto written = write_file_synced(temporary, bytes);
    if (!written) {
        return written;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        remove_if_present(temporary);
        return Result<void>::failure(build_error(
            ErrorCode::io_error,
            fmt::format(
                "could not publish quantized-core staging artifact: {}",
                filesystem_error.message()),
            path));
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::vector<std::uint16_t>> load_quantized_core_artifact(
    const std::filesystem::path& path,
    const LunarTileKey expected_key,
    const Sha256Digest& expected_dependency_hash) {
    constexpr std::size_t header_bytes = 56U;
    constexpr std::size_t expected_samples =
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
    auto file = read_file(path);
    if (!file) {
        return Result<std::vector<std::uint16_t>>::failure(std::move(file).error());
    }
    const ByteView bytes = file.value();
    if (bytes.size() != header_bytes + expected_samples * 2U ||
        std::string_view{reinterpret_cast<const char*>(bytes.data()), 4} != "LTQC" ||
        read_le_u16(bytes, 4) != 1 || read_le_u16(bytes, 6) != 0 ||
        read_le_u64(bytes, 8) != expected_key.encoded() ||
        !std::ranges::equal(
            bytes.subspan(16, expected_dependency_hash.bytes.size()),
            expected_dependency_hash.bytes) ||
        read_le_u32(bytes, 48) != expected_samples) {
        return failure<std::vector<std::uint16_t>>(
            ErrorCode::hash_mismatch,
            "quantized-core staging identity does not match",
            path);
    }
    Bytes checksum_bytes = file.value();
    const std::uint32_t stored_crc = read_le_u32(bytes, 52);
    write_u32(checksum_bytes, 52, 0);
    if (crc32c(checksum_bytes) != stored_crc) {
        return failure<std::vector<std::uint16_t>>(
            ErrorCode::checksum_mismatch,
            "quantized-core staging artifact CRC does not match",
            path);
    }
    std::vector<std::uint16_t> samples;
    samples.reserve(expected_samples);
    for (std::size_t offset = header_bytes; offset < bytes.size(); offset += 2U) {
        samples.push_back(read_le_u16(bytes, offset));
    }
    return Result<std::vector<std::uint16_t>>::success(std::move(samples));
}

[[nodiscard]] Result<RasterTileWork> build_raster_tile_work(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const BuildOptions& options,
    const LunarTileKey key,
    const bool require_core_for_reuse = true) {
    auto selected_sources = active_sources_for_tile(key, sources);
    if (!selected_sources) {
        return Result<RasterTileWork>::failure(std::move(selected_sources).error());
    }
    std::vector<std::size_t> active_sources = std::move(selected_sources).value();

    std::vector<DatasetArtifact> active_datasets;
    std::vector<std::optional<Sha256Digest>> window_dependencies;
    active_datasets.reserve(active_sources.size());
    window_dependencies.reserve(active_sources.size());
    for (const std::size_t source_index : active_sources) {
        auto window = sources[source_index]->WindowDependency(key);
        if (!window) {
            return Result<RasterTileWork>::failure(std::move(window).error());
        }
        active_datasets.push_back(datasets[source_index]);
        window_dependencies.push_back(window.value());
    }
    auto dependency = tile_dependency_hash(
        key, identity, active_datasets, window_dependencies, "heterogeneous_fusion_v1");
    if (!dependency) {
        return Result<RasterTileWork>::failure(std::move(dependency).error());
    }

    const std::filesystem::path core_path = staged_elevation_artifact_path(
        configuration.cache_directory / "staging", key, dependency.value());
    const std::filesystem::path encoded_path = encoded_tile_artifact_path(
        configuration, key, dependency.value());
    if (options.incremental) {
        auto reusable = cache.FindReusableTile(key, dependency.value());
        if (!reusable) {
            return Result<RasterTileWork>::failure(std::move(reusable).error());
        }
        if (reusable.value()) {
            auto encoded = load_encoded_tile_artifact(
                reusable.value()->staging_path,
                key,
                dependency.value(),
                reusable.value()->content_hash);
            auto core = require_core_for_reuse
                ? load_staged_elevation_tile(core_path, key, dependency.value())
                : Result<StagedElevationTile>::success(StagedElevationTile{
                      key, dependency.value(), {}, core_path});
            if (encoded && core) {
                ElevationSampler apron_sampler = [
                    &configuration, sources, datasets, active_sources](const QscCoordinate qsc) {
                    return sample_fused_point(qsc, configuration, sources, datasets, active_sources);
                };
                return Result<RasterTileWork>::success(RasterTileWork{
                    std::move(core).value(),
                    {},
                    std::move(apron_sampler),
                    {},
                    std::move(encoded).value(),
                });
            }
        }
    }

    auto marked = cache.MarkTileBuilding(key, dependency.value(), encoded_path);
    if (!marked) {
        return Result<RasterTileWork>::failure(std::move(marked).error());
    }
    if (options.cancellation_token().stop_requested()) {
        return Result<RasterTileWork>::failure(Error{
            ErrorCode::cancelled, "terrain build was cancelled"}.with_tile_key(key.encoded()));
    }

    double coarsest_resolution = 0.0;
    double finest_resolution = std::numeric_limits<double>::infinity();
    for (const std::size_t source_index : active_sources) {
        coarsest_resolution = std::max(
            coarsest_resolution, datasets[source_index].effective_resolution_meters);
        finest_resolution = std::min(
            finest_resolution, datasets[source_index].effective_resolution_meters);
    }
    const std::uint32_t maximum_passes = residual_filter_pass_count(
        coarsest_resolution, finest_resolution);
    const std::uint32_t halo = 33U + 2U * maximum_passes;
    const std::uint32_t grid_width = format_v1::core_vertices + 2U * halo;
    const std::uint32_t grid_height = grid_width;
    const std::uint64_t cells_per_axis =
        std::uint64_t{format_v1::tile_cells} * (std::uint64_t{1} << key.level());
    const std::int64_t first_x =
        static_cast<std::int64_t>(key.x()) * format_v1::tile_cells - halo;
    const std::int64_t first_y =
        static_cast<std::int64_t>(key.y()) * format_v1::tile_cells - halo;

    std::vector<LunarGeodeticCoordinate> coordinates;
    coordinates.reserve(std::size_t{grid_width} * grid_height);
    {
        TelemetryActivity projection_activity{options.execution.telemetry, "projection"};
        for (std::uint32_t y = 0; y < grid_height; ++y) {
            if (options.cancellation_token().stop_requested()) {
                return Result<RasterTileWork>::failure(Error{
                    ErrorCode::cancelled, "terrain build was cancelled"}.with_tile_key(
                        key.encoded()));
            }
            for (std::uint32_t x = 0; x < grid_width; ++x) {
                const double u = (
                    2.0 * static_cast<double>(first_x + x) -
                    static_cast<double>(cells_per_axis)) /
                    static_cast<double>(cells_per_axis);
                const double v = (
                    2.0 * static_cast<double>(first_y + y) -
                    static_cast<double>(cells_per_axis)) /
                    static_cast<double>(cells_per_axis);
                auto mapped = map_extended_raster_coordinate(QscCoordinate{
                    static_cast<QscFace>(key.face()), u, v, 0.0});
                if (!mapped) {
                    return Result<RasterTileWork>::failure(std::move(mapped).error());
                }
                auto coordinate = QscProjection::Inverse(mapped.value());
                if (!coordinate) {
                    return Result<RasterTileWork>::failure(std::move(coordinate).error());
                }
                coordinates.push_back(coordinate.value());
            }
        }
    }

    std::vector<FusionGridSource> fusion_sources;
    fusion_sources.reserve(active_sources.size());
    {
        TelemetryActivity sampling_activity{options.execution.telemetry, "sampling"};
        for (const std::size_t source_index : active_sources) {
        FusionGridSource fusion_source;
        fusion_source.dataset_id = datasets[source_index].id;
        fusion_source.priority = configuration.rasters[source_index].priority;
        fusion_source.policy = configuration.rasters[source_index].fusion_policy;
        fusion_source.native_resolution_meters = datasets[source_index].nominal_resolution_meters;
        fusion_source.width = grid_width;
        fusion_source.height = grid_height;
        fusion_source.samples.resize(coordinates.size());
        fusion_source.quality.assign(coordinates.size(), 0);
        auto sampled = sources[source_index]->SampleBatch(
            coordinates, options.cancellation_token());
        if (!sampled) {
            return Result<RasterTileWork>::failure(std::move(sampled).error());
        }
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->AddCount(
                "requested_source_samples", coordinates.size());
            const std::uint64_t core_samples =
                std::uint64_t{format_v1::core_vertices} * format_v1::core_vertices;
            if (coordinates.size() > core_samples) {
                options.execution.telemetry->AddCount(
                    "requested_halo_samples", coordinates.size() - core_samples);
            }
        }
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            if (options.cancellation_token().stop_requested()) {
                return Result<RasterTileWork>::failure(Error{
                    ErrorCode::cancelled, "terrain build was cancelled"}.with_tile_key(key.encoded()));
            }
            if (!sampled.value()[index]) {
                continue;
            }
            fusion_source.samples[index] = sampled.value()[index]->elevation_meters;
            fusion_source.quality[index] |= sampled.value()[index]->quality_flags;
            if (sampled.value()[index]->interpolated) {
                fusion_source.quality[index] |= quality_interpolated;
            }
            if (sampled.value()[index]->filled_no_data) {
                fusion_source.quality[index] |= quality_filled_no_data;
            }
        }
            fusion_sources.push_back(std::move(fusion_source));
        }
    }
    auto fused = [&]() {
        TelemetryActivity fusion_activity{options.execution.telemetry, "fusion"};
        return fuse_source_grids(fusion_sources);
    }();
    if (!fused) {
        Error error = std::move(fused).error();
        error.with_tile_key(key.encoded());
        return Result<RasterTileWork>::failure(std::move(error));
    }
    auto summary = summarize_fused_core(fused.value(), halo, halo);
    if (!summary) {
        return Result<RasterTileWork>::failure(std::move(summary).error());
    }
    std::vector<double> core_samples;
    core_samples.reserve(
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices);
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        const std::size_t offset = std::size_t{y + halo} * grid_width + halo;
        core_samples.insert(
            core_samples.end(),
            fused.value().elevations.begin() + static_cast<std::ptrdiff_t>(offset),
            fused.value().elevations.begin() + static_cast<std::ptrdiff_t>(
                offset + format_v1::core_vertices));
    }
    auto staged = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return stage_elevation_tile_samples(
            key, dependency.value(), configuration.cache_directory / "staging", core_samples);
    }();
    if (!staged) {
        return Result<RasterTileWork>::failure(std::move(staged).error());
    }
    record_staging_file(options.execution.telemetry, staged.value().artifact_path);

    auto halo_elevations = std::make_shared<std::vector<double>>(
        std::move(fused).value().elevations);
    std::vector<double> virtual_apron_samples;
    virtual_apron_samples.reserve(
        4U * format_v1::core_vertices + 4U);
    const auto halo_at = [&](const std::uint32_t x, const std::uint32_t y) {
        return (*halo_elevations)[std::size_t{y} * grid_width + x];
    };
    for (std::uint32_t parameter = 0; parameter < format_v1::core_vertices;
         ++parameter) {
        virtual_apron_samples.push_back(halo_at(halo - 1U, halo + parameter));
    }
    for (std::uint32_t parameter = 0; parameter < format_v1::core_vertices;
         ++parameter) {
        virtual_apron_samples.push_back(halo_at(halo + format_v1::core_vertices, halo + parameter));
    }
    for (std::uint32_t parameter = 0; parameter < format_v1::core_vertices;
         ++parameter) {
        virtual_apron_samples.push_back(halo_at(halo + parameter, halo - 1U));
    }
    for (std::uint32_t parameter = 0; parameter < format_v1::core_vertices;
         ++parameter) {
        virtual_apron_samples.push_back(halo_at(halo + parameter, halo + format_v1::core_vertices));
    }
    virtual_apron_samples.push_back(halo_at(halo - 1U, halo - 1U));
    virtual_apron_samples.push_back(
        halo_at(halo + format_v1::core_vertices, halo - 1U));
    virtual_apron_samples.push_back(
        halo_at(halo - 1U, halo + format_v1::core_vertices));
    virtual_apron_samples.push_back(halo_at(
        halo + format_v1::core_vertices,
        halo + format_v1::core_vertices));
    ElevationSampler apron_sampler = [&configuration, sources, datasets, key, first_x, first_y,
                                      grid_width, grid_height, cells_per_axis, halo_elevations,
                                      active_sources](
                                         const QscCoordinate qsc) -> Result<double> {
        if (static_cast<std::uint8_t>(qsc.face) == key.face()) {
            const auto grid_x = static_cast<std::int64_t>(std::llround(
                (qsc.u + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - first_x;
            const auto grid_y = static_cast<std::int64_t>(std::llround(
                (qsc.v + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - first_y;
            if (grid_x >= 0 && grid_y >= 0 &&
                grid_x < static_cast<std::int64_t>(grid_width) &&
                grid_y < static_cast<std::int64_t>(grid_height)) {
                return Result<double>::success((*halo_elevations)[
                    static_cast<std::size_t>(grid_y) * grid_width +
                    static_cast<std::size_t>(grid_x)]);
            }
        }
        return sample_fused_point(qsc, configuration, sources, datasets, active_sources);
    };
    return Result<RasterTileWork>::success(RasterTileWork{
        std::move(staged).value(),
        std::move(summary).value(),
        std::move(apron_sampler),
        std::move(virtual_apron_samples),
        std::nullopt,
    });
}

[[nodiscard]] Result<RasterTileRecord> prepare_streaming_raster_tile(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const BuildOptions& options,
    const LunarTileKey key) {
    auto work = build_raster_tile_work(
        configuration, identity, sources, datasets, cache, options, key, false);
    if (!work) {
        return Result<RasterTileRecord>::failure(std::move(work).error());
    }
    RasterTileRecord record{
        key,
        work.value().staged.dependency_hash,
        work.value().staged.artifact_path,
        raster_auxiliary_artifact_path(
            configuration, key, work.value().staged.dependency_hash),
        quantized_core_artifact_path(
            configuration, key, work.value().staged.dependency_hash),
        std::move(work.value().reused_tile),
    };
    if (record.reused_tile) {
        record.reused_tile->payload_bytes = static_cast<std::uint32_t>(
            record.reused_tile->payload.size());
        record.reused_tile->payload.clear();
        record.reused_tile->payload.shrink_to_fit();
        return Result<RasterTileRecord>::success(std::move(record));
    }
    auto persisted = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return persist_raster_auxiliary_artifact(
            record.auxiliary_path,
            key,
            record.dependency_hash,
            RasterTileAuxiliary{
                std::move(work.value().summary),
                std::move(work.value().virtual_apron_samples)});
    }();
    if (!persisted) {
        return Result<RasterTileRecord>::failure(std::move(persisted).error());
    }
    record_staging_file(options.execution.telemetry, record.auxiliary_path);
    return Result<RasterTileRecord>::success(std::move(record));
}

[[nodiscard]] Result<StagedElevationTile> load_streaming_staged_tile(
    const RasterTileRecord& record) {
    std::error_code filesystem_error;
    if (std::filesystem::exists(record.staged_core_path, filesystem_error)) {
        return load_staged_elevation_tile(
            record.staged_core_path, record.key, record.dependency_hash);
    }
    if (filesystem_error) {
        return Result<StagedElevationTile>::failure(build_error(
            ErrorCode::io_error,
            fmt::format(
                "could not inspect staged raster core: {}",
                filesystem_error.message()),
            record.staged_core_path));
    }
    auto quantized = load_quantized_core_artifact(
        record.quantized_core_path, record.key, record.dependency_hash);
    if (!quantized) {
        return Result<StagedElevationTile>::failure(std::move(quantized).error());
    }
    std::vector<double> samples;
    samples.reserve(quantized.value().size());
    for (const std::uint16_t code : quantized.value()) {
        samples.push_back(-16'384.0 + static_cast<double>(code) * 0.5);
    }
    return Result<StagedElevationTile>::success(StagedElevationTile{
        record.key,
        record.dependency_hash,
        std::move(samples),
        record.quantized_core_path,
    });
}

[[nodiscard]] Result<ElevationSampler> make_streaming_apron_sampler(
    const BuilderConfiguration& configuration,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    const LunarTileKey key,
    std::vector<double> apron_samples) {
    constexpr std::size_t edge_samples = format_v1::core_vertices;
    constexpr std::size_t corner_offset = 4U * edge_samples;
    if (apron_samples.size() != corner_offset + 4U) {
        return failure<ElevationSampler>(
            ErrorCode::invalid_argument,
            "streaming apron sampler does not contain four edges and corners");
    }
    auto selected_sources = active_sources_for_tile(key, sources);
    if (!selected_sources) {
        return Result<ElevationSampler>::failure(std::move(selected_sources).error());
    }
    const std::uint64_t cells_per_axis =
        std::uint64_t{format_v1::tile_cells} * (std::uint64_t{1} << key.level());
    const std::int64_t origin_x =
        static_cast<std::int64_t>(key.x()) * format_v1::tile_cells;
    const std::int64_t origin_y =
        static_cast<std::int64_t>(key.y()) * format_v1::tile_cells;
    ElevationSampler sampler = [
        &configuration,
        sources,
        datasets,
        key,
        cells_per_axis,
        origin_x,
        origin_y,
        apron_samples = std::move(apron_samples),
        active_sources = std::move(selected_sources).value()](
            const QscCoordinate qsc) -> Result<double> {
        if (static_cast<std::uint8_t>(qsc.face) == key.face()) {
            const std::int64_t x = static_cast<std::int64_t>(std::llround(
                (qsc.u + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - origin_x;
            const std::int64_t y = static_cast<std::int64_t>(std::llround(
                (qsc.v + 1.0) * 0.5 * static_cast<double>(cells_per_axis))) - origin_y;
            constexpr std::int64_t outside = format_v1::core_vertices;
            const auto at = [&](const std::size_t index) {
                return Result<double>::success(apron_samples[index]);
            };
            if (x == -1 && y == -1) {
                return at(corner_offset);
            }
            if (x == outside && y == -1) {
                return at(corner_offset + 1U);
            }
            if (x == -1 && y == outside) {
                return at(corner_offset + 2U);
            }
            if (x == outside && y == outside) {
                return at(corner_offset + 3U);
            }
            if (x == -1 && y >= 0 && y < outside) {
                return at(static_cast<std::size_t>(y));
            }
            if (x == outside && y >= 0 && y < outside) {
                return at(edge_samples + static_cast<std::size_t>(y));
            }
            if (y == -1 && x >= 0 && x < outside) {
                return at(2U * edge_samples + static_cast<std::size_t>(x));
            }
            if (y == outside && x >= 0 && x < outside) {
                return at(3U * edge_samples + static_cast<std::size_t>(x));
            }
        }
        return sample_fused_point(
            qsc, configuration, sources, datasets, active_sources);
    };
    return Result<ElevationSampler>::success(std::move(sampler));
}

[[nodiscard]] Result<FinalizedElevationTile> finalize_streaming_raster_tile(
    const BuilderConfiguration& configuration,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    const std::span<const RasterTileRecord> records,
    const std::map<std::uint64_t, std::size_t>& records_by_key,
    const std::size_t target_index,
    const BuildOptions& options) {
    const RasterTileRecord& target = records[target_index];
    std::set<std::uint64_t> neighborhood{target.key.encoded()};
    std::vector<LunarTileKey> frontier{target.key};
    constexpr std::array edges{
        QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    for (std::uint32_t depth = 0; depth < 2U; ++depth) {
        if (options.cancellation_token().stop_requested()) {
            return Result<FinalizedElevationTile>::failure(Error{
                ErrorCode::cancelled, "terrain build was cancelled"}.with_tile_key(
                    target.key.encoded()));
        }
        std::vector<LunarTileKey> next;
        for (const LunarTileKey key : frontier) {
            for (const QscEdge edge : edges) {
                auto neighbor = qsc_tile_neighbor(key, edge);
                if (!neighbor) {
                    return Result<FinalizedElevationTile>::failure(
                        std::move(neighbor).error());
                }
                const auto found = records_by_key.find(neighbor.value().key.encoded());
                if (found != records_by_key.end() &&
                    records[found->second].key.level() == target.key.level() &&
                    neighborhood.insert(neighbor.value().key.encoded()).second) {
                    next.push_back(neighbor.value().key);
                }
            }
        }
        frontier = std::move(next);
    }

    std::vector<StagedElevationTile> staged;
    std::vector<ElevationSampler> samplers;
    staged.reserve(neighborhood.size());
    samplers.reserve(neighborhood.size());
    std::size_t local_target = 0;
    {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        for (const std::uint64_t encoded : neighborhood) {
        if (options.cancellation_token().stop_requested()) {
            return Result<FinalizedElevationTile>::failure(Error{
                ErrorCode::cancelled, "terrain build was cancelled"}.with_tile_key(
                    target.key.encoded()));
        }
        const std::size_t record_index = records_by_key.at(encoded);
        auto loaded = load_streaming_staged_tile(records[record_index]);
        if (!loaded) {
            return Result<FinalizedElevationTile>::failure(std::move(loaded).error());
        }
        if (record_index == target_index) {
            local_target = staged.size();
            auto auxiliary = load_raster_auxiliary_artifact(
                target.auxiliary_path, target.key, target.dependency_hash);
            if (!auxiliary) {
                return Result<FinalizedElevationTile>::failure(
                    std::move(auxiliary).error());
            }
            auto sampler = make_streaming_apron_sampler(
                configuration,
                sources,
                datasets,
                target.key,
                std::move(auxiliary).value().virtual_apron_samples);
            if (!sampler) {
                return Result<FinalizedElevationTile>::failure(
                    std::move(sampler).error());
            }
            samplers.push_back(std::move(sampler).value());
        } else {
            samplers.emplace_back([](const QscCoordinate) {
                return Result<double>::success(0.0);
            });
        }
            staged.push_back(std::move(loaded).value());
        }
    }
    auto resolved = [&]() {
        TelemetryActivity seam_activity{options.execution.telemetry, "seam_resolution"};
        return resolve_elevation_boundaries(staged);
    }();
    if (!resolved) {
        return Result<FinalizedElevationTile>::failure(std::move(resolved).error());
    }
    auto finalized = [&]() {
        TelemetryActivity quantization_activity{options.execution.telemetry, "quantization"};
        return finalize_elevation_tiles(staged, samplers);
    }();
    if (!finalized) {
        return Result<FinalizedElevationTile>::failure(std::move(finalized).error());
    }
    return Result<FinalizedElevationTile>::success(
        std::move(finalized).value()[local_target]);
}

[[nodiscard]] Result<EncodedTile> encode_streaming_raster_tile(
    const BuilderConfiguration& configuration,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const std::span<const RasterTileRecord> records,
    const std::map<std::uint64_t, std::size_t>& records_by_key,
    const std::span<const std::uint8_t> child_masks,
    const std::size_t target_index,
    const BuildOptions& options) {
    const RasterTileRecord& target = records[target_index];
    auto finalized = finalize_streaming_raster_tile(
        configuration, sources, datasets, records, records_by_key, target_index, options);
    if (!finalized) {
        return Result<EncodedTile>::failure(std::move(finalized).error());
    }
    auto auxiliary = load_raster_auxiliary_artifact(
        target.auxiliary_path, target.key, target.dependency_hash);
    if (!auxiliary) {
        return Result<EncodedTile>::failure(std::move(auxiliary).error());
    }
    double geometric_error = 0.0;
    if (target.key.level() != 0) {
        const std::optional<LunarTileKey> parent = target.key.parent();
        if (!parent) {
            return failure<EncodedTile>(
                ErrorCode::invalid_argument,
                "streaming hierarchy tile has no parent");
        }
        const auto found = records_by_key.find(parent->encoded());
        if (found == records_by_key.end()) {
            return Result<EncodedTile>::failure(Error{
                ErrorCode::invalid_argument,
                "streaming hierarchy tile has no materialized parent"}.with_tile_key(
                    target.key.encoded()));
        }
        const RasterTileRecord& parent_record = records[found->second];
        auto parent_core = load_quantized_core_artifact(
            parent_record.quantized_core_path,
            parent_record.key,
            parent_record.dependency_hash);
        if (!parent_core) {
            return Result<EncodedTile>::failure(std::move(parent_core).error());
        }
        auto measured = geometric_error_bilinear_u16_v1(
            target.key,
            finalized.value().quantized_core,
            parent_record.key,
            parent_core.value());
        if (!measured) {
            return Result<EncodedTile>::failure(std::move(measured).error());
        }
        geometric_error = measured.value();
    }
    auto core_persisted = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return persist_quantized_core_artifact(
            target.quantized_core_path,
            target.key,
            target.dependency_hash,
            finalized.value().quantized_core);
    }();
    if (!core_persisted) {
        return Result<EncodedTile>::failure(std::move(core_persisted).error());
    }
    record_staging_file(options.execution.telemetry, target.quantized_core_path);
    auto encoded = [&]() {
        TelemetryActivity encoding_activity{options.execution.telemetry, "encoding"};
        return encode_tile(
            target.key,
            finalized.value().serialized_samples,
            target.dependency_hash,
            datasets,
            auxiliary.value().summary,
            geometric_error,
            child_masks[target_index]);
    }();
    if (!encoded) {
        return encoded;
    }
    const std::filesystem::path encoded_path = encoded_tile_artifact_path(
        configuration, target.key, target.dependency_hash);
    auto persisted = [&]() {
        TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
        return persist_encoded_tile_artifact(encoded_path, encoded.value());
    }();
    if (!persisted) {
        return Result<EncodedTile>::failure(std::move(persisted).error());
    }
    record_staging_file(options.execution.telemetry, encoded_path);
    auto stored = [&]() {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        return cache.StoreCompletedTile(
            target.key,
            target.dependency_hash,
            encoded.value().content_hash,
            encoded_path);
    }();
    if (!stored) {
        return Result<EncodedTile>::failure(std::move(stored).error());
    }
    encoded.value().artifact_path = encoded_path;
    encoded.value().payload_bytes = static_cast<std::uint32_t>(encoded.value().payload.size());
    encoded.value().payload.clear();
    encoded.value().payload.shrink_to_fit();
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->AddCount("tiles_quantized");
        options.execution.telemetry->AddCount("tiles_encoded");
    }
    return encoded;
}

[[nodiscard]] Result<TileBuildResult> build_raster_tiles_streaming(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const BuildOptions& options,
    const SparseHierarchyPlan& hierarchy) {
    std::vector<RasterTileRecord> records;
    records.reserve(hierarchy.tiles.size());
    for (const LunarTileKey key : hierarchy.tiles) {
        records.push_back(RasterTileRecord{key, {}, {}, {}, {}, std::nullopt});
    }
    std::map<std::uint64_t, std::size_t> records_by_key;
    for (std::size_t index = 0; index < hierarchy.tiles.size(); ++index) {
        records_by_key.emplace(hierarchy.tiles[index].encoded(), index);
    }
    auto child_masks = materialized_child_masks(hierarchy.tiles);
    if (!child_masks) {
        return Result<TileBuildResult>::failure(std::move(child_masks).error());
    }
    std::vector<std::optional<EncodedTile>> encoded_slots(hierarchy.tiles.size());
    TileBuildResult result;
    result.tiles.reserve(hierarchy.tiles.size());
    std::uint64_t completed_tiles = 0;

    for (std::uint8_t level = 0; level <= configuration.maximum_level; ++level) {
        if (options.cancellation_token().stop_requested()) {
            return Result<TileBuildResult>::failure(Error{
                ErrorCode::cancelled, "terrain build was cancelled"});
        }
        std::vector<std::size_t> level_indices;
        for (std::size_t index = 0; index < hierarchy.tiles.size(); ++index) {
            if (hierarchy.tiles[index].level() == level) {
                level_indices.push_back(index);
            }
        }
        if (level_indices.empty()) {
            continue;
        }
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetPhase(
                "tile_preparation",
                completed_tiles,
                hierarchy.tiles.size(),
                std::nullopt,
                level,
                level);
        }
        std::vector<std::optional<RasterTileRecord>> prepared(level_indices.size());
        std::vector<BuilderTask> preparation_tasks;
        preparation_tasks.reserve(level_indices.size());
        for (std::size_t position = 0; position < level_indices.size(); ++position) {
            preparation_tasks.emplace_back([&, position]() -> Result<void> {
                const std::size_t index = level_indices[position];
                auto record = prepare_streaming_raster_tile(
                    configuration,
                    identity,
                    sources,
                    datasets,
                    cache,
                    options,
                    hierarchy.tiles[index]);
                if (!record) {
                    return Result<void>::failure(std::move(record).error());
                }
                prepared[position] = std::move(record).value();
                return Result<void>::success();
            });
        }
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetWorkers(
                (std::min)(configuration.worker_threads,
                           static_cast<std::uint32_t>(preparation_tasks.size())),
                0,
                preparation_tasks.size());
        }
        auto prepared_result = run_bounded_tasks(
            preparation_tasks, configuration.worker_threads, options.cancellation_token());
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetWorkers(0, 0, 0);
        }
        if (!prepared_result) {
            return Result<TileBuildResult>::failure(std::move(prepared_result).error());
        }
        for (std::size_t position = 0; position < level_indices.size(); ++position) {
            if (!prepared[position]) {
                return failure<TileBuildResult>(
                    ErrorCode::internal_error,
                    "streaming raster preparation omitted a tile record");
            }
            records[level_indices[position]] = std::move(*prepared[position]);
        }

        std::vector<BuilderTask> encoding_tasks;
        encoding_tasks.reserve(level_indices.size());
        for (const std::size_t index : level_indices) {
            if (records[index].reused_tile) {
                encoded_slots[index] = std::move(records[index].reused_tile);
                ++result.reused_tile_count;
                continue;
            }
            encoding_tasks.emplace_back([&, index]() -> Result<void> {
                auto encoded = encode_streaming_raster_tile(
                    configuration,
                    sources,
                    datasets,
                    cache,
                    records,
                    records_by_key,
                    child_masks.value(),
                    index,
                    options);
                if (!encoded) {
                    return Result<void>::failure(std::move(encoded).error());
                }
                encoded_slots[index] = std::move(encoded).value();
                return Result<void>::success();
            });
        }
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetPhase(
                "tile_encoding",
                completed_tiles,
                hierarchy.tiles.size(),
                std::nullopt,
                level,
                level);
            options.execution.telemetry->SetWorkers(
                (std::min)(configuration.worker_threads,
                           static_cast<std::uint32_t>(encoding_tasks.size())),
                0,
                encoding_tasks.size());
        }
        auto encoded_result = run_bounded_tasks(
            encoding_tasks, configuration.worker_threads, options.cancellation_token());
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetWorkers(0, 0, 0);
        }
        if (!encoded_result) {
            return Result<TileBuildResult>::failure(std::move(encoded_result).error());
        }
        result.built_tile_count += encoding_tasks.size();
        completed_tiles += level_indices.size();
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->SetWork(completed_tiles, hierarchy.tiles.size());
        }

        for (const std::size_t index : level_indices) {
            if (records[index].reused_tile) {
                continue;
            }
            std::error_code filesystem_error;
            std::filesystem::remove(records[index].staged_core_path, filesystem_error);
            if (!filesystem_error) {
                std::filesystem::remove(records[index].auxiliary_path, filesystem_error);
            }
            if (filesystem_error) {
                return Result<TileBuildResult>::failure(build_error(
                    ErrorCode::io_error,
                    fmt::format(
                        "could not release bounded raster staging data: {}",
                        filesystem_error.message()),
                    records[index].staged_core_path));
            }
        }
    }
    for (std::optional<EncodedTile>& encoded : encoded_slots) {
        if (!encoded) {
            return failure<TileBuildResult>(
                ErrorCode::internal_error,
                "streaming raster build omitted an encoded tile");
        }
        result.tiles.push_back(std::move(*encoded));
    }
    return Result<TileBuildResult>::success(std::move(result));
}

[[nodiscard]] Result<TileBuildResult> build_raster_tiles(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const IRasterSource* const> sources,
    const std::span<const DatasetArtifact> datasets,
    BuildCache& cache,
    const BuildOptions& options) {
    if (sources.empty() || sources.size() != configuration.rasters.size() ||
        sources.size() != datasets.size()) {
        return failure<TileBuildResult>(
            ErrorCode::invalid_argument, "raster fusion sources are inconsistent");
    }
    auto hierarchy = raster_hierarchy_plan(configuration, sources, datasets);
    if (!hierarchy) {
        return Result<TileBuildResult>::failure(std::move(hierarchy).error());
    }
    constexpr std::size_t streaming_tile_threshold = 16U;
    if (hierarchy.value().tiles.size() > streaming_tile_threshold) {
        return build_raster_tiles_streaming(
            configuration,
            identity,
            sources,
            datasets,
            cache,
            options,
            hierarchy.value());
    }

    std::vector<std::optional<RasterTileWork>> work_slots(hierarchy.value().tiles.size());
    std::vector<BuilderTask> tasks;
    tasks.reserve(hierarchy.value().tiles.size());
    for (std::size_t index = 0; index < hierarchy.value().tiles.size(); ++index) {
        tasks.emplace_back([&, index]() -> Result<void> {
            auto work = build_raster_tile_work(
                configuration,
                identity,
                sources,
                datasets,
                cache,
                options,
                hierarchy.value().tiles[index]);
            if (!work) {
                return Result<void>::failure(std::move(work).error());
            }
            work_slots[index] = std::move(work).value();
            return Result<void>::success();
        });
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWorkers(
            (std::min)(configuration.worker_threads, static_cast<std::uint32_t>(tasks.size())),
            0,
            tasks.size());
    }
    auto executed = run_bounded_tasks(
        tasks, configuration.worker_threads, options.cancellation_token());
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWorkers(0, 0, 0);
    }
    if (!executed) {
        return Result<TileBuildResult>::failure(std::move(executed).error());
    }

    std::vector<RasterTileWork> works;
    std::vector<StagedElevationTile> staged;
    std::vector<ElevationSampler> samplers;
    works.reserve(work_slots.size());
    staged.reserve(work_slots.size());
    samplers.reserve(work_slots.size());
    for (std::optional<RasterTileWork>& slot : work_slots) {
        if (!slot) {
            return failure<TileBuildResult>(
                ErrorCode::internal_error, "bounded raster tile task omitted a result");
        }
        works.push_back(std::move(*slot));
    }
    for (RasterTileWork& work : works) {
        staged.push_back(std::move(work.staged));
        samplers.push_back(std::move(work.apron_sampler));
    }
    auto resolved = [&]() {
        TelemetryActivity seam_activity{options.execution.telemetry, "seam_resolution"};
        return resolve_elevation_boundaries(staged);
    }();
    if (!resolved) {
        return Result<TileBuildResult>::failure(std::move(resolved).error());
    }
    auto finalized = [&]() {
        TelemetryActivity quantization_activity{options.execution.telemetry, "quantization"};
        return finalize_elevation_tiles(staged, samplers);
    }();
    if (!finalized) {
        return Result<TileBuildResult>::failure(std::move(finalized).error());
    }
    auto hierarchy_metadata = compute_hierarchy_metadata(finalized.value());
    if (!hierarchy_metadata) {
        return Result<TileBuildResult>::failure(std::move(hierarchy_metadata).error());
    }

    TileBuildResult result;
    result.tiles.reserve(finalized.value().size());
    TelemetryActivity encoding_activity{options.execution.telemetry, "encoding"};
    for (std::size_t index = 0; index < finalized.value().size(); ++index) {
        if (works[index].reused_tile) {
            result.tiles.push_back(std::move(*works[index].reused_tile));
            ++result.reused_tile_count;
            continue;
        }
        const FinalizedElevationTile& elevation = finalized.value()[index];
        auto tile = encode_tile(
            elevation.key,
            elevation.serialized_samples,
            elevation.dependency_hash,
            datasets,
            works[index].summary,
            hierarchy_metadata.value()[index].geometric_error_meters,
            hierarchy_metadata.value()[index].materialized_child_mask);
        if (!tile) {
            return Result<TileBuildResult>::failure(std::move(tile).error());
        }
        const std::filesystem::path encoded_path = encoded_tile_artifact_path(
            configuration, elevation.key, elevation.dependency_hash);
        auto persisted = [&]() {
            TelemetryActivity staging_activity{options.execution.telemetry, "staging"};
            return persist_encoded_tile_artifact(encoded_path, tile.value());
        }();
        if (!persisted) {
            return Result<TileBuildResult>::failure(std::move(persisted).error());
        }
        record_staging_file(options.execution.telemetry, encoded_path);
        auto stored = [&]() {
            TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
            return cache.StoreCompletedTile(
                elevation.key,
                elevation.dependency_hash,
                tile.value().content_hash,
                encoded_path);
        }();
        if (!stored) {
            return Result<TileBuildResult>::failure(std::move(stored).error());
        }
        result.tiles.push_back(std::move(tile).value());
        ++result.built_tile_count;
        if (options.execution.telemetry != nullptr) {
            options.execution.telemetry->AddCount("tiles_encoded");
            options.execution.telemetry->SetWork(index + 1U, finalized.value().size());
        }
    }
    return Result<TileBuildResult>::success(std::move(result));
}

[[nodiscard]] Result<BuildReport> publish_build(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const std::span<const DatasetArtifact> datasets,
    const Sha256Digest& registry_hash,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles,
    const std::uint64_t built_tile_count,
    const std::uint64_t reused_tile_count,
    BuildCache* const cache,
    const BuildOptions& options) {
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("packing", 0, tiles.size());
    }
    auto checked = check_execution(options.execution);
    if (!checked) {
        return Result<BuildReport>::failure(std::move(checked).error());
    }
    auto packs = [&]() {
        TelemetryActivity activity{options.execution.telemetry, "packing"};
        return build_packs(configuration, database_id, tiles, options);
    }();
    if (!packs) {
        return Result<BuildReport>::failure(std::move(packs).error());
    }
    auto database = make_database_file(
        identity, datasets, registry_hash, database_id, tiles, packs.value(), options);
    if (!database) {
        return Result<BuildReport>::failure(std::move(database).error());
    }
    const std::filesystem::path database_path =
        configuration.output_directory / (configuration.database_name + ".ltdb");
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("publication", 0, packs.value().size() + 1U);
    }
    auto published = [&]() {
        TelemetryActivity activity{options.execution.telemetry, "publication"};
        return publish_outputs(database_path, database.value().first, packs.value(), options);
    }();
    if (!published) {
        return Result<BuildReport>::failure(std::move(published).error());
    }
    if (cache != nullptr) {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        std::uint64_t packing_updates = 0;
        for (const PackArtifact& pack : packs.value()) {
            for (const PackTilePlacement& placement : pack.placements) {
                if ((packing_updates % 64U) == 0U) {
                    checked = check_build_execution(options);
                    if (!checked) {
                        return Result<BuildReport>::failure(std::move(checked).error());
                    }
                }
                auto updated = cache->UpdatePacking(
                    tiles[placement.tile_index].key,
                    pack.id,
                    placement.payload_offset);
                if (!updated) {
                    return Result<BuildReport>::failure(std::move(updated).error());
                }
                ++packing_updates;
            }
        }
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetWork(packs.value().size() + 1U, packs.value().size() + 1U);
        auto checkpoint = options.execution.telemetry->Checkpoint("published-database");
        if (!checkpoint) {
            return Result<BuildReport>::failure(std::move(checkpoint).error());
        }
    }

    BuildReport report;
    report.database_path = database_path;
    report.database_content_hash = database.value().second;
    report.builder_configuration_hash = identity.builder_hash;
    report.tile_count = tiles.size();
    report.built_tile_count = built_tile_count;
    report.reused_tile_count = reused_tile_count;
    report.packs.reserve(packs.value().size());
    for (const PackArtifact& pack : packs.value()) {
        report.packs.push_back(PackBuildReport{
            pack.id,
            configuration.output_directory / pack.relative_path,
            pack.hash,
            pack.file_bytes,
        });
    }
    return Result<BuildReport>::success(std::move(report));
}

}  // namespace

Result<BuildReport> build_synthetic(
    const BuilderConfiguration& configuration,
    const BuildOptions options) {
    auto checked = check_execution(options.execution);
    if (!checked) {
        return Result<BuildReport>::failure(std::move(checked).error());
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<BuildReport>::failure(std::move(identity).error());
    }
    auto dataset = make_synthetic_dataset(configuration, identity.value());
    if (!dataset) {
        return Result<BuildReport>::failure(std::move(dataset).error());
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("sqlite");
    }
    auto cache = [&]() {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        return BuildCache::Open(configuration.cache_directory);
    }();
    if (!cache) {
        return Result<BuildReport>::failure(std::move(cache).error());
    }
    auto recorded_dataset = [&]() {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        return cache.value().RecordDataset(
            dataset.value().id, dataset.value().artifact_bundle_hash);
    }();
    if (!recorded_dataset) {
        return Result<BuildReport>::failure(std::move(recorded_dataset).error());
    }
    const std::array datasets{dataset.value()};
    auto registry_hash = dataset_registry_hash(datasets);
    if (!registry_hash) {
        return Result<BuildReport>::failure(std::move(registry_hash).error());
    }
    auto database_id = make_database_id(identity.value().builder_hash, registry_hash.value());
    if (!database_id) {
        return Result<BuildReport>::failure(std::move(database_id).error());
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("sampling", 0, 6);
    }
    auto tiles = build_tiles(
        configuration, identity.value(), dataset.value(), cache.value(), options);
    if (!tiles) {
        return Result<BuildReport>::failure(std::move(tiles).error());
    }
    return publish_build(
        configuration,
        identity.value(),
        datasets,
        registry_hash.value(),
        database_id.value(),
        tiles.value().tiles,
        tiles.value().built_tile_count,
        tiles.value().reused_tile_count,
        &cache.value(),
        options);
}

Result<BuildReport> build_raster_source(
    const BuilderConfiguration& configuration,
    const BuildOptions options) {
    auto checked = check_execution(options.execution);
    if (!checked) {
        return Result<BuildReport>::failure(std::move(checked).error());
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<BuildReport>::failure(std::move(identity).error());
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("source_catalog", 0, configuration.rasters.size());
    }
    std::vector<std::unique_ptr<IRasterSource>> owned_sources;
    const std::vector<std::unique_ptr<IRasterSource>>* sources = nullptr;
    if (options.execution.prepared_source_catalog != nullptr) {
        if (options.execution.prepared_source_catalog->identity.builder_hash !=
                identity.value().builder_hash ||
            options.execution.prepared_source_catalog->sources.size() !=
                configuration.rasters.size()) {
            return Result<BuildReport>::failure(Error{
                ErrorCode::invalid_argument,
                "prepared source catalog does not match the configuration"});
        }
        sources = &options.execution.prepared_source_catalog->sources;
    } else {
        auto opened = open_raster_sources(
            configuration, identity.value(), options.execution.telemetry);
        if (!opened) {
            return Result<BuildReport>::failure(std::move(opened).error());
        }
        owned_sources = std::move(opened).value();
        sources = &owned_sources;
    }
    std::vector<const IRasterSource*> source_pointers;
    std::vector<DatasetArtifact> datasets;
    source_pointers.reserve(sources->size());
    datasets.reserve(sources->size());
    for (const auto& source : *sources) {
        source_pointers.push_back(source.get());
        datasets.push_back(source->metadata());
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("sqlite");
    }
    auto cache = [&]() {
        TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
        return BuildCache::Open(configuration.cache_directory);
    }();
    if (!cache) {
        return Result<BuildReport>::failure(std::move(cache).error());
    }
    for (const DatasetArtifact& dataset : datasets) {
        auto recorded = [&]() {
            TelemetryActivity sqlite_activity{options.execution.telemetry, "sqlite"};
            return cache.value().RecordDataset(
                dataset.id, dataset.artifact_bundle_hash);
        }();
        if (!recorded) {
            return Result<BuildReport>::failure(std::move(recorded).error());
        }
    }
    if (options.execution.telemetry != nullptr) {
        options.execution.telemetry->SetPhase("tile_pipeline");
    }
    auto tiles = configuration.materialize_hierarchy
        ? build_raster_tiles(
              configuration,
              identity.value(),
              source_pointers,
              datasets,
              cache.value(),
              options)
        : build_raster_prototype_tiles(
              configuration,
              identity.value(),
              source_pointers,
              datasets,
              cache.value(),
              options);
    if (!tiles) {
        return Result<BuildReport>::failure(std::move(tiles).error());
    }
    std::ranges::sort(datasets, {}, [](const DatasetArtifact& dataset) {
        return dataset.id.value;
    });
    auto registry_hash = dataset_registry_hash(datasets);
    if (!registry_hash) {
        return Result<BuildReport>::failure(std::move(registry_hash).error());
    }
    auto database_id = make_database_id(identity.value().builder_hash, registry_hash.value());
    if (!database_id) {
        return Result<BuildReport>::failure(std::move(database_id).error());
    }
    return publish_build(
        configuration,
        identity.value(),
        datasets,
        registry_hash.value(),
        database_id.value(),
        tiles.value().tiles,
        tiles.value().built_tile_count,
        tiles.value().reused_tile_count,
        &cache.value(),
        options);
}

}  // namespace lunar::terrain::builder
