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
};

struct TileBuildResult {
    std::vector<EncodedTile> tiles;
    std::uint64_t built_tile_count{};
    std::uint64_t reused_tile_count{};
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
            auto staged = stage_elevation_tile(
                keys[index],
                dependencies[index],
                configuration.cache_directory / "staging",
                sampler);
            if (!staged) {
                return Result<void>::failure(std::move(staged).error());
            }
            staged_slots[index] = std::move(staged).value();
            return Result<void>::success();
        });
    }
    auto executed = run_bounded_tasks(tasks, configuration.worker_threads, options.cancellation);
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
    auto resolved = resolve_elevation_boundaries(staged);
    if (!resolved) {
        return Result<TileBuildResult>::failure(std::move(resolved).error());
    }
    auto finalized = finalize_elevation_tiles(staged, sampler);
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
            auto stored = cache.StoreCompletedTile(
                elevation.key,
                elevation.dependency_hash,
                tile.value().content_hash,
                encoded_path);
            if (!stored) {
                return Result<TileBuildResult>::failure(std::move(stored).error());
            }
            ++result.built_tile_count;
        }
        result.tiles.push_back(std::move(tile).value());
    }
    return Result<TileBuildResult>::success(std::move(result));
}

[[nodiscard]] Result<std::vector<PackArtifact>> build_packs(
    const BuilderConfiguration& configuration,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles) {
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
    for (const EncodedTile& tile : tiles) {
        packing_tiles.push_back(PackingTile{tile.key, tile.payload.size()});
    }
    auto ranges = plan_canonical_pack_ranges(packing_tiles, configuration.target_pack_bytes);
    if (!ranges) {
        return Result<std::vector<PackArtifact>>::failure(std::move(ranges).error());
    }
    for (const CanonicalPackRange range : ranges.value()) {
        const std::uint32_t pack_number = static_cast<std::uint32_t>(packs.size());
        const std::uint8_t face = tiles[range.first_tile].key.face();
        const std::uint8_t level = tiles[range.first_tile].key.level();
        Bytes bytes(format_v1::bytes::pack_header);
        std::vector<PackTilePlacement> placements;
        placements.reserve(range.tile_count);
        for (std::size_t tile_index = range.first_tile;
             tile_index < range.first_tile + range.tile_count;
             ++tile_index) {
            const EncodedTile& tile = tiles[tile_index];
            const std::uint64_t payload_offset = align8(bytes.size());
            append_zeroes(
                bytes, static_cast<std::size_t>(payload_offset - bytes.size()));
            append_bytes(bytes, tile.payload);
            placements.push_back(PackTilePlacement{
                tile_index,
                payload_offset,
                static_cast<std::uint32_t>(tile.payload.size()),
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
        packs.push_back(PackArtifact{
            PackId{pack_number},
            relative_path,
            std::move(bytes),
            pack_hash.value(),
            first_key,
            last_key,
            std::move(placements),
        });
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
        write_u64(bytes, offset + format_v1::pack_record_offset::file_bytes, pack.bytes.size());
        write_u64(bytes, offset + format_v1::pack_record_offset::first_tile_key, pack.first_key.encoded());
        write_u64(bytes, offset + format_v1::pack_record_offset::last_tile_key, pack.last_key.encoded());
        write_bytes(bytes, offset + format_v1::pack_record_offset::sha256, pack.hash.bytes);
    }
    return bytes;
}

[[nodiscard]] Bytes make_tile_index_chunk(
    const std::vector<PackArtifact>& packs,
    const std::vector<EncodedTile>& tiles) {
    Bytes bytes(tiles.size() * format_v1::bytes::tile_index_record);
    for (const PackArtifact& pack : packs) {
        for (const PackTilePlacement& placement : pack.placements) {
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
    return bytes;
}

[[nodiscard]] Result<Sha256Digest> database_content_hash(
    const ConfigurationIdentity& identity,
    const std::vector<ChunkArtifact>& chunks,
    const std::vector<PackArtifact>& packs) {
    Bytes input;
    append_domain(input, "LTDB_DATABASE_CONTENT_V1");
    append_u16(input, format_v1::major_version);
    append_u16(input, format_v1::minor_version);
    append_bytes(input, identity.builder_hash.bytes);
    append_u32(input, static_cast<std::uint32_t>(chunks.size()));
    for (const ChunkArtifact& chunk : chunks) {
        append_text(input, std::string_view{chunk.tag.data(), chunk.tag.size()});
        append_u16(input, 1);
        append_u16(input, mandatory_chunk_flags);
        append_u64(input, chunk.bytes.size());
        append_bytes(input, chunk.bytes);
    }
    append_u32(input, static_cast<std::uint32_t>(packs.size()));
    for (const PackArtifact& pack : packs) {
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
    const std::vector<PackArtifact>& packs) {
    auto strings = make_string_table(datasets, packs);
    if (!strings) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(strings).error());
    }
    const Bytes tidx = make_tile_index_chunk(packs, tiles);
    auto tidx_hash = framed_text_hash("LTDB_TILE_INDEX_V1", std::string_view{
        reinterpret_cast<const char*>(tidx.data()), tidx.size()});
    if (!tidx_hash) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(tidx_hash).error());
    }

    std::vector<ChunkArtifact> chunks{
        ChunkArtifact{{'D', 'S', 'E', 'T'}, make_dataset_chunk(datasets, strings.value()), 0},
        ChunkArtifact{{'M', 'E', 'T', 'A'}, make_meta_chunk(datasets), 0},
        ChunkArtifact{{'P', 'A', 'C', 'K'}, make_pack_chunk(packs, strings.value()), 0},
        ChunkArtifact{{'S', 'T', 'R', 'S'}, std::move(strings).value().bytes, 0},
        ChunkArtifact{{'T', 'I', 'D', 'X'}, tidx, 0},
    };
    auto content_hash = database_content_hash(identity, chunks, packs);
    if (!content_hash) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(content_hash).error());
    }

    const std::uint64_t directory_bytes = chunks.size() * format_v1::bytes::chunk_directory_entry;
    Bytes file(static_cast<std::size_t>(align8(format_v1::bytes::ltdb_header + directory_bytes)));
    for (ChunkArtifact& chunk : chunks) {
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
    const ByteView bytes) {
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
    while (offset < bytes.size()) {
        const std::size_t remaining = bytes.size() - offset;
        const DWORD requested = static_cast<DWORD>(std::min<std::size_t>(
            remaining, std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (WriteFile(handle, bytes.data() + offset, requested, &written, nullptr) == FALSE ||
            written != requested) {
            succeeded = false;
            break;
        }
        offset += written;
    }
    if (succeeded && FlushFileBuffers(handle) == FALSE) {
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
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            succeeded = false;
            break;
        }
        offset += static_cast<std::size_t>(written);
    }
    if (succeeded && ::fsync(descriptor) != 0) {
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
#endif
    return Result<void>::success();
}

[[nodiscard]] Result<Bytes> read_file(const std::filesystem::path& path) {
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
    if (!bytes.empty()) {
        stream.read(reinterpret_cast<char*>(bytes.data()), size);
    }
    if (!stream) {
        return failure<Bytes>(ErrorCode::io_error, "could not read existing output", path);
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
    });
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
    const std::vector<PackArtifact>& packs) {
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

    for (const PackArtifact& pack : packs) {
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
        auto written = write_file_synced(temporary, pack.bytes);
        if (!written) {
            cleanup();
            return written;
        }
    }
    const std::filesystem::path database_temporary = temporary_sibling(database_path);
    temporary_paths.push_back(database_temporary);
    auto database_written = write_file_synced(database_temporary, database_bytes);
    if (!database_written) {
        cleanup();
        return database_written;
    }

    for (const PackArtifact& pack : packs) {
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
        auto existing = read_file(final_path);
        if (!existing) {
            cleanup();
            return Result<void>::failure(std::move(existing).error());
        }
        if (existing.value() != pack.bytes) {
            cleanup();
            return Result<void>::failure(build_error(
                ErrorCode::hash_mismatch,
                "an existing content-addressed pack has different bytes",
                final_path));
        }
    }

    for (const PackArtifact& pack : packs) {
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

    if (std::filesystem::exists(database_path, filesystem_error)) {
        auto existing = read_file(database_path);
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
    if (options.cancellation.stop_requested()) {
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
    for (std::size_t source_index = 0; source_index < sources.size(); ++source_index) {
        if (options.cancellation.stop_requested()) {
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
            if (options.cancellation.stop_requested()) {
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
                if (sample.value()->interpolated) {
                    fusion_source.quality[index] |= quality_interpolated;
                }
                if (sample.value()->filled_no_data) {
                    fusion_source.quality[index] |= quality_filled_no_data;
                }
            }
        }
        fusion_sources.push_back(std::move(fusion_source));
    }
    auto fused = fuse_source_grids(fusion_sources);
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
    auto staged = stage_elevation_tile_samples(
        key.value(),
        dependency.value(),
        configuration.cache_directory / "staging",
        core_samples);
    if (!staged) {
        return Result<TileBuildResult>::failure(std::move(staged).error());
    }
    std::vector<StagedElevationTile> staged_tiles;
    staged_tiles.push_back(std::move(staged).value());
    auto resolved = resolve_elevation_boundaries(staged_tiles);
    if (!resolved) {
        return Result<TileBuildResult>::failure(std::move(resolved).error());
    }
    auto finalized = finalize_elevation_tiles(staged_tiles, sampler);
    if (!finalized) {
        return Result<TileBuildResult>::failure(std::move(finalized).error());
    }
    auto tile = encode_tile(
        key.value(),
        finalized.value().front().serialized_samples,
        dependency.value(),
        datasets,
        summary.value());
    if (!tile) {
        return Result<TileBuildResult>::failure(std::move(tile).error());
    }
    auto persisted = persist_encoded_tile_artifact(encoded_path, tile.value());
    if (!persisted) {
        return Result<TileBuildResult>::failure(std::move(persisted).error());
    }
    auto stored = cache.StoreCompletedTile(
        key.value(), dependency.value(), tile.value().content_hash, encoded_path);
    if (!stored) {
        return Result<TileBuildResult>::failure(std::move(stored).error());
    }
    TileBuildResult result;
    result.tiles.push_back(std::move(tile).value());
    result.built_tile_count = 1;
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
    BuildCache* const cache) {
    auto packs = build_packs(configuration, database_id, tiles);
    if (!packs) {
        return Result<BuildReport>::failure(std::move(packs).error());
    }
    auto database = make_database_file(
        identity, datasets, registry_hash, database_id, tiles, packs.value());
    if (!database) {
        return Result<BuildReport>::failure(std::move(database).error());
    }
    const std::filesystem::path database_path =
        configuration.output_directory / (configuration.database_name + ".ltdb");
    auto published = publish_outputs(database_path, database.value().first, packs.value());
    if (!published) {
        return Result<BuildReport>::failure(std::move(published).error());
    }
    if (cache != nullptr) {
        for (const PackArtifact& pack : packs.value()) {
            for (const PackTilePlacement& placement : pack.placements) {
                auto updated = cache->UpdatePacking(
                    tiles[placement.tile_index].key,
                    pack.id,
                    placement.payload_offset);
                if (!updated) {
                    return Result<BuildReport>::failure(std::move(updated).error());
                }
            }
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
            pack.bytes.size(),
        });
    }
    return Result<BuildReport>::success(std::move(report));
}

}  // namespace

Result<BuildReport> build_synthetic(
    const BuilderConfiguration& configuration,
    const BuildOptions options) {
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<BuildReport>::failure(std::move(identity).error());
    }
    auto dataset = make_synthetic_dataset(configuration, identity.value());
    if (!dataset) {
        return Result<BuildReport>::failure(std::move(dataset).error());
    }
    auto cache = BuildCache::Open(configuration.cache_directory);
    if (!cache) {
        return Result<BuildReport>::failure(std::move(cache).error());
    }
    auto recorded_dataset = cache.value().RecordDataset(
        dataset.value().id, dataset.value().artifact_bundle_hash);
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
        &cache.value());
}

Result<BuildReport> build_raster_source(
    const BuilderConfiguration& configuration,
    const BuildOptions options) {
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<BuildReport>::failure(std::move(identity).error());
    }
    auto sources = open_raster_sources(configuration, identity.value());
    if (!sources) {
        return Result<BuildReport>::failure(std::move(sources).error());
    }
    std::vector<const IRasterSource*> source_pointers;
    std::vector<DatasetArtifact> datasets;
    source_pointers.reserve(sources.value().size());
    datasets.reserve(sources.value().size());
    for (const auto& source : sources.value()) {
        source_pointers.push_back(source.get());
        datasets.push_back(source->metadata());
    }
    auto cache = BuildCache::Open(configuration.cache_directory);
    if (!cache) {
        return Result<BuildReport>::failure(std::move(cache).error());
    }
    for (const DatasetArtifact& dataset : datasets) {
        auto recorded = cache.value().RecordDataset(
            dataset.id, dataset.artifact_bundle_hash);
        if (!recorded) {
            return Result<BuildReport>::failure(std::move(recorded).error());
        }
    }
    auto tiles = build_raster_tiles(
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
        &cache.value());
}

}  // namespace lunar::terrain::builder
