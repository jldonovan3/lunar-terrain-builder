#include "builder/builder.hpp"

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
#include <lunar/terrain/qsc_topology.hpp>

namespace lunar::terrain::builder {
namespace {

using Bytes = std::vector<std::byte>;
using ByteView = std::span<const std::byte>;

constexpr std::uint32_t mandatory_chunk_flags = 0x0003U;
constexpr std::uint32_t required_channel = 0x0001U;
constexpr std::uint32_t tile_has_provenance = 0x0001U;
constexpr std::uint32_t synthetic_resolution_meters = 10'000U;
constexpr std::size_t core_sample_count =
    std::size_t{format_v1::core_vertices} * format_v1::core_vertices;
constexpr std::size_t serialized_sample_count =
    std::size_t{format_v1::serialized_elevation_samples} *
    format_v1::serialized_elevation_samples;

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
};

struct PackArtifact {
    PackId id;
    std::filesystem::path relative_path;
    Bytes bytes;
    Sha256Digest hash;
    LunarTileKey first_key;
    LunarTileKey last_key;
    std::uint64_t payload_offset{};
    std::uint32_t payload_bytes{};
    std::size_t tile_index{};
};

struct SyntheticDataset {
    DatasetId id;
    std::array<std::string, 8> strings;
    std::string artifact_name;
    Bytes artifact_bytes;
    Sha256Digest artifact_hash;
    Sha256Digest artifact_bundle_hash;
    std::string metadata_json;
    Sha256Digest registry_hash;
};

struct ChunkArtifact {
    std::array<char, 4> tag{};
    Bytes bytes;
    std::uint64_t file_offset{};
};

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

void append_f32(Bytes& bytes, const float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
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
    for (const unsigned char character : value) {
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

[[nodiscard]] Result<SyntheticDataset> make_dataset(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity) {
    SyntheticDataset dataset;
    dataset.id = identity.synthetic_dataset_id;
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
    dataset.artifact_name = "synthetic/p0-v1.json";
    const std::string descriptor = fmt::format(
        "{{\"amplitude_meters\":{},\"formula\":\"analytic_unit_vector_v1\",\"version\":1}}",
        configuration.synthetic_amplitude_meters);
    dataset.artifact_bytes.assign(
        std::as_bytes(std::span{descriptor}).begin(),
        std::as_bytes(std::span{descriptor}).end());
    auto artifact_hash = sha256(dataset.artifact_bytes);
    if (!artifact_hash) {
        return Result<SyntheticDataset>::failure(std::move(artifact_hash).error());
    }
    dataset.artifact_hash = artifact_hash.value();

    Bytes bundle_input;
    append_domain(bundle_input, "LTDB_ARTIFACT_BUNDLE_V1");
    append_u32(bundle_input, 1);
    append_u32(bundle_input, static_cast<std::uint32_t>(dataset.artifact_name.size()));
    append_text(bundle_input, dataset.artifact_name);
    append_u64(bundle_input, dataset.artifact_bytes.size());
    append_bytes(bundle_input, dataset.artifact_hash.bytes);
    auto bundle_hash = sha256(bundle_input);
    if (!bundle_hash) {
        return Result<SyntheticDataset>::failure(std::move(bundle_hash).error());
    }
    dataset.artifact_bundle_hash = bundle_hash.value();

    dataset.metadata_json = fmt::format(
        "{{\"artifact_members\":[{{\"bytes\":{},\"name\":{},\"sha256\":{}}}],"
        "\"datum\":{{\"reference_radius_m\":1737400}},"
        "\"elevation_representation\":\"elevation_meters\",\"metadata_overrides\":{{}},"
        "\"no_data\":\"none\",\"stable_key\":{}}}",
        dataset.artifact_bytes.size(),
        json_string(dataset.artifact_name),
        json_string(dataset.artifact_hash.to_hex()),
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
    append_f64(registry_input, static_cast<double>(synthetic_resolution_meters));
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, 0x7FF8000000000000ULL);
    append_u64(registry_input, dataset.artifact_bytes.size());
    append_bytes(registry_input, dataset.artifact_bundle_hash.bytes);
    append_u64(registry_input, dataset.metadata_json.size());
    append_text(registry_input, dataset.metadata_json);
    append_u32(registry_input, 0);
    auto registry_hash = sha256(registry_input);
    if (!registry_hash) {
        return Result<SyntheticDataset>::failure(std::move(registry_hash).error());
    }
    dataset.registry_hash = registry_hash.value();
    return Result<SyntheticDataset>::success(std::move(dataset));
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

[[nodiscard]] Result<std::uint16_t> quantize_elevation(const double elevation) {
    if (!std::isfinite(elevation)) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument, "synthetic sampler produced a non-finite elevation");
    }
    const double scaled = (elevation - (-16'384.0)) / 0.5;
    if (scaled < 0.0 || scaled > 65'535.0) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument, "synthetic elevation is outside the v1 U16 profile");
    }
    const double lower = std::floor(scaled);
    const double fraction = scaled - lower;
    std::uint32_t rounded = static_cast<std::uint32_t>(lower);
    if (fraction > 0.5 || (fraction == 0.5 && (rounded & 1U) != 0)) {
        ++rounded;
    }
    if (rounded > std::numeric_limits<std::uint16_t>::max()) {
        return failure<std::uint16_t>(
            ErrorCode::invalid_argument, "rounded synthetic elevation is outside U16");
    }
    return Result<std::uint16_t>::success(static_cast<std::uint16_t>(rounded));
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

[[nodiscard]] constexpr std::size_t core_index(
    const std::uint16_t x,
    const std::uint16_t y) noexcept {
    return std::size_t{y} * format_v1::core_vertices + x;
}

[[nodiscard]] constexpr std::size_t edge_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    switch (edge) {
        case QscEdge::west:
            return core_index(0, parameter);
        case QscEdge::east:
            return core_index(format_v1::core_vertices - 1U, parameter);
        case QscEdge::south:
            return core_index(parameter, 0);
        case QscEdge::north:
            return core_index(parameter, format_v1::core_vertices - 1U);
    }
    return 0;
}

[[nodiscard]] constexpr std::size_t interior_edge_index(
    const QscEdge edge,
    const std::uint16_t parameter) noexcept {
    switch (edge) {
        case QscEdge::west:
            return core_index(1, parameter);
        case QscEdge::east:
            return core_index(format_v1::core_vertices - 2U, parameter);
        case QscEdge::south:
            return core_index(parameter, 1);
        case QscEdge::north:
            return core_index(parameter, format_v1::core_vertices - 2U);
    }
    return 0;
}

[[nodiscard]] Result<std::array<std::vector<std::uint16_t>, 6>> build_quantized_cores(
    const BuilderConfiguration& configuration) {
    std::array<std::vector<std::uint16_t>, 6> cores;
    for (std::uint8_t face = 0; face < cores.size(); ++face) {
        auto& core = cores[face];
        core.resize(core_sample_count);
        for (std::uint16_t y = 0; y < format_v1::core_vertices; ++y) {
            auto v = QscProjection::LatticeCoordinate(0, y, 0);
            if (!v) {
                return Result<std::array<std::vector<std::uint16_t>, 6>>::failure(
                    std::move(v).error());
            }
            for (std::uint16_t x = 0; x < format_v1::core_vertices; ++x) {
                auto u = QscProjection::LatticeCoordinate(0, x, 0);
                if (!u) {
                    return Result<std::array<std::vector<std::uint16_t>, 6>>::failure(
                        std::move(u).error());
                }
                auto coordinate = QscProjection::Inverse(QscCoordinate{
                    static_cast<QscFace>(face), u.value(), v.value(), 0.0});
                if (!coordinate) {
                    return Result<std::array<std::vector<std::uint16_t>, 6>>::failure(
                        std::move(coordinate).error());
                }
                auto quantized = quantize_elevation(synthetic_elevation(
                    coordinate.value(), configuration.synthetic_amplitude_meters));
                if (!quantized) {
                    Error error = std::move(quantized).error();
                    auto key = LunarTileKey::create(face, 0, 0, 0);
                    if (key) {
                        error.with_tile_key(key.value().encoded());
                    }
                    return Result<std::array<std::vector<std::uint16_t>, 6>>::failure(
                        std::move(error));
                }
                core[core_index(x, y)] = quantized.value();
            }
        }
    }

    constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    for (std::uint8_t face = 0; face < cores.size(); ++face) {
        for (const QscEdge edge : edges) {
            const auto source_face = static_cast<QscFace>(face);
            if (!qsc_face_owns_edge(source_face, edge)) {
                continue;
            }
            const QscEdgeConnection connection = qsc_edge_connection(source_face, edge);
            auto& destination = cores[static_cast<std::size_t>(connection.face)];
            for (std::uint16_t parameter = 0; parameter < format_v1::core_vertices; ++parameter) {
                const std::uint16_t mapped = connection.reversed
                    ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                    : parameter;
                destination[edge_index(connection.edge, mapped)] =
                    cores[face][edge_index(edge, parameter)];
            }
        }
    }
    return Result<std::array<std::vector<std::uint16_t>, 6>>::success(std::move(cores));
}

[[nodiscard]] std::vector<std::uint16_t> add_apron(
    const std::array<std::vector<std::uint16_t>, 6>& cores,
    const std::uint8_t face) {
    std::vector<std::uint16_t> samples(serialized_sample_count);
    constexpr std::uint16_t stored_width = format_v1::serialized_elevation_samples;
    const auto stored_index = [](const std::uint16_t x, const std::uint16_t y) {
        return std::size_t{y} * format_v1::serialized_elevation_samples + x;
    };
    for (std::uint16_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::uint16_t x = 0; x < format_v1::core_vertices; ++x) {
            samples[stored_index(static_cast<std::uint16_t>(x + 1U), static_cast<std::uint16_t>(y + 1U))] =
                cores[face][core_index(x, y)];
        }
    }

    constexpr std::array edges{QscEdge::west, QscEdge::east, QscEdge::south, QscEdge::north};
    for (const QscEdge edge : edges) {
        const QscEdgeConnection connection = qsc_edge_connection(static_cast<QscFace>(face), edge);
        const auto& neighbor = cores[static_cast<std::size_t>(connection.face)];
        for (std::uint16_t parameter = 0; parameter < format_v1::core_vertices; ++parameter) {
            const std::uint16_t mapped = connection.reversed
                ? static_cast<std::uint16_t>(format_v1::core_vertices - 1U - parameter)
                : parameter;
            const std::uint16_t value = neighbor[interior_edge_index(connection.edge, mapped)];
            switch (edge) {
                case QscEdge::west:
                    samples[stored_index(0, static_cast<std::uint16_t>(parameter + 1U))] = value;
                    break;
                case QscEdge::east:
                    samples[stored_index(stored_width - 1U, static_cast<std::uint16_t>(parameter + 1U))] = value;
                    break;
                case QscEdge::south:
                    samples[stored_index(static_cast<std::uint16_t>(parameter + 1U), 0)] = value;
                    break;
                case QscEdge::north:
                    samples[stored_index(static_cast<std::uint16_t>(parameter + 1U), stored_width - 1U)] = value;
                    break;
            }
        }
    }
    samples[stored_index(0, 0)] = cores[face][core_index(0, 0)];
    samples[stored_index(stored_width - 1U, 0)] =
        cores[face][core_index(format_v1::core_vertices - 1U, 0)];
    samples[stored_index(0, stored_width - 1U)] =
        cores[face][core_index(0, format_v1::core_vertices - 1U)];
    samples[stored_index(stored_width - 1U, stored_width - 1U)] =
        cores[face][core_index(format_v1::core_vertices - 1U, format_v1::core_vertices - 1U)];
    return samples;
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

[[nodiscard]] Bytes provenance_bytes(const DatasetId dataset_id) {
    Bytes bytes(format_v1::bytes::provenance_header + format_v1::bytes::provenance_palette_entry);
    write_u16(bytes, format_v1::provenance_header_offset::version, 1);
    write_u16(bytes, format_v1::provenance_header_offset::palette_count, 1);
    write_u32(bytes, format_v1::bytes::provenance_header, dataset_id.value);
    write_f32(bytes, format_v1::bytes::provenance_header + 8U, 1.0F);
    write_f32(
        bytes,
        format_v1::bytes::provenance_header + 12U,
        static_cast<float>(synthetic_resolution_meters));
    return bytes;
}

[[nodiscard]] Result<Sha256Digest> tile_dependency_hash(
    const LunarTileKey key,
    const ConfigurationIdentity& identity,
    const SyntheticDataset& dataset) {
    const std::string semantic_hex = identity.semantic_hash.to_hex();
    const std::string jcs = fmt::format(
        "{{\"builder_algorithm_version\":1,\"datum_version\":\"synthetic_datum_v1\","
        "\"fusion\":{{\"algorithm\":\"synthetic_analytic_v1\",\"configuration_sha256\":\"{}\"}},"
        "\"projection\":{{\"id\":1,\"implementation_version\":1}},"
        "\"quantization\":{{\"configuration_sha256\":\"{}\",\"id\":1}},"
        "\"semantic_configuration_sha256\":\"{}\",\"sources\":[{{"
        "\"artifact_bundle_sha256\":\"{}\",\"dataset_id\":{},\"windows\":[]}}],"
        "\"tile_key\":\"{:016x}\",\"tile_schema_version\":1}}",
        semantic_hex,
        semantic_hex,
        semantic_hex,
        dataset.artifact_bundle_hash.to_hex(),
        dataset.id.value,
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
    const ConfigurationIdentity& identity,
    const SyntheticDataset& dataset) {
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
    provenance.decoded = provenance_bytes(dataset.id);
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

    auto dependency_hash = tile_dependency_hash(key, identity, dataset);
    if (!dependency_hash) {
        return Result<EncodedTile>::failure(std::move(dependency_hash).error());
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
    write_u32(payload, format_v1::tile_header_offset::flags, tile_has_provenance);
    write_u16(payload, format_v1::tile_header_offset::channel_count, static_cast<std::uint16_t>(channels.size()));
    write_u16(payload, format_v1::tile_header_offset::tile_cells, format_v1::tile_cells);
    write_u16(payload, format_v1::tile_header_offset::core_vertices, format_v1::core_vertices);
    write_u8(payload, format_v1::tile_header_offset::apron, format_v1::apron_samples);
    write_u8(payload, format_v1::tile_header_offset::encoding_profile, static_cast<std::uint8_t>(EncodingProfile::global_u16));
    write_f32(payload, format_v1::tile_header_offset::effective_resolution, static_cast<float>(synthetic_resolution_meters));
    write_f32(payload, format_v1::tile_header_offset::geometric_error, 0.0F);
    write_f32(payload, format_v1::tile_header_offset::minimum_elevation, -16'384.0F + static_cast<float>(*minimum) * 0.5F);
    write_f32(payload, format_v1::tile_header_offset::maximum_elevation, -16'384.0F + static_cast<float>(*maximum) * 0.5F);
    write_u32(payload, format_v1::tile_header_offset::primary_dataset, dataset.id.value);
    write_u16(payload, format_v1::tile_header_offset::provenance_palette_count, 1);
    write_u32(payload, format_v1::tile_header_offset::channel_directory_offset, format_v1::bytes::tile_header);
    write_u32(payload, format_v1::tile_header_offset::channel_directory_bytes, static_cast<std::uint32_t>(directory_bytes));
    write_u32(payload, format_v1::tile_header_offset::data_region_offset, static_cast<std::uint32_t>(data_offset));
    write_bytes(payload, format_v1::tile_header_offset::dependency_hash, ByteView{dependency_hash.value().bytes}.first<16>());
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
        dependency_hash.value(),
        content_hash.value(),
        0,
        static_cast<std::uint32_t>(logical_sum),
    };
    encoded.payload_crc = crc32c(encoded.payload);
    return Result<EncodedTile>::success(std::move(encoded));
}

[[nodiscard]] Result<std::vector<EncodedTile>> build_tiles(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    const SyntheticDataset& dataset) {
    auto cores = build_quantized_cores(configuration);
    if (!cores) {
        return Result<std::vector<EncodedTile>>::failure(std::move(cores).error());
    }
    std::vector<EncodedTile> tiles;
    tiles.reserve(6);
    for (std::uint8_t face = 0; face < 6; ++face) {
        auto key = LunarTileKey::create(face, 0, 0, 0);
        if (!key) {
            return Result<std::vector<EncodedTile>>::failure(std::move(key).error());
        }
        auto tile = encode_tile(key.value(), add_apron(cores.value(), face), identity, dataset);
        if (!tile) {
            return Result<std::vector<EncodedTile>>::failure(std::move(tile).error());
        }
        tiles.push_back(std::move(tile).value());
    }
    return Result<std::vector<EncodedTile>>::success(std::move(tiles));
}

[[nodiscard]] Result<std::vector<PackArtifact>> build_packs(
    const BuilderConfiguration& configuration,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles) {
    std::vector<PackArtifact> packs;
    packs.reserve(tiles.size());
    const std::string id_hex = database_id_hex(database_id);
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const EncodedTile& tile = tiles[index];
        Bytes bytes(format_v1::bytes::pack_header);
        const std::uint64_t payload_offset = bytes.size();
        append_bytes(bytes, tile.payload);
        write_text(bytes, format_v1::pack_header_offset::magic, "LTPK");
        write_u16(bytes, format_v1::pack_header_offset::major, format_v1::major_version);
        write_u16(bytes, format_v1::pack_header_offset::minor, format_v1::minor_version);
        write_u32(bytes, format_v1::pack_header_offset::header_bytes, format_v1::bytes::pack_header);
        write_u32(bytes, format_v1::pack_header_offset::endian, format_v1::endian_tag);
        write_u32(bytes, format_v1::pack_header_offset::pack_id, static_cast<std::uint32_t>(index));
        write_u64(bytes, format_v1::pack_header_offset::tile_count, 1);
        write_u64(bytes, format_v1::pack_header_offset::payload_region_offset, format_v1::bytes::pack_header);
        write_u64(bytes, format_v1::pack_header_offset::file_bytes, bytes.size());
        auto pack_hash = sha256(bytes);
        if (!pack_hash) {
            return Result<std::vector<PackArtifact>>::failure(std::move(pack_hash).error());
        }
        write_bytes(bytes, format_v1::pack_header_offset::sha256_prefix, ByteView{pack_hash.value().bytes}.first<16>());
        const std::filesystem::path relative_path = fmt::format(
            "Packs/{}_{}_F{}_L00_P0000.ltp",
            configuration.database_name,
            id_hex,
            tile.key.face());
        packs.push_back(PackArtifact{
            PackId{static_cast<std::uint32_t>(index)},
            relative_path,
            std::move(bytes),
            pack_hash.value(),
            tile.key,
            tile.key,
            payload_offset,
            static_cast<std::uint32_t>(tile.payload.size()),
            index,
        });
    }
    return Result<std::vector<PackArtifact>>::success(std::move(packs));
}

struct StringTable {
    Bytes bytes;
    std::map<std::string, std::uint32_t, decltype(&unsigned_utf8_less)> offsets{&unsigned_utf8_less};
};

[[nodiscard]] Result<StringTable> make_string_table(
    const SyntheticDataset& dataset,
    const std::vector<PackArtifact>& packs) {
    std::vector<std::string> values;
    for (const std::string& value : dataset.strings) {
        if (!value.empty()) {
            values.push_back(value);
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
    const SyntheticDataset& dataset,
    const StringTable& strings) {
    Bytes bytes(format_v1::bytes::dataset_record);
    write_u32(bytes, format_v1::dataset_record_offset::dataset_id, dataset.id.value);
    for (std::size_t index = 0; index < dataset.strings.size(); ++index) {
        write_u32(
            bytes,
            format_v1::dataset_record_offset::first_string_id + index * 4U,
            strings.offsets.at(dataset.strings[index]));
    }
    write_f64(bytes, format_v1::dataset_record_offset::nominal_resolution, static_cast<double>(synthetic_resolution_meters));
    write_u64(bytes, format_v1::dataset_record_offset::horizontal_accuracy, 0x7FF8000000000000ULL);
    write_u64(bytes, format_v1::dataset_record_offset::vertical_accuracy, 0x7FF8000000000000ULL);
    write_u64(bytes, format_v1::dataset_record_offset::source_no_data, 0x7FF8000000000000ULL);
    write_u64(bytes, format_v1::dataset_record_offset::artifact_bundle_bytes, dataset.artifact_bytes.size());
    write_bytes(bytes, format_v1::dataset_record_offset::artifact_bundle_hash, dataset.artifact_bundle_hash.bytes);
    write_u32(bytes, format_v1::dataset_record_offset::meta_offset, 0);
    write_u32(bytes, format_v1::dataset_record_offset::meta_bytes, static_cast<std::uint32_t>(dataset.metadata_json.size()));
    return bytes;
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
        write_u64(bytes, offset + format_v1::pack_record_offset::tile_count, 1);
        write_u64(bytes, offset + format_v1::pack_record_offset::file_bytes, pack.bytes.size());
        write_u64(bytes, offset + format_v1::pack_record_offset::first_tile_key, pack.first_key.encoded());
        write_u64(bytes, offset + format_v1::pack_record_offset::last_tile_key, pack.last_key.encoded());
        write_bytes(bytes, offset + format_v1::pack_record_offset::sha256, pack.hash.bytes);
    }
    return bytes;
}

[[nodiscard]] Bytes make_tile_index_chunk(
    const std::vector<PackArtifact>& packs,
    const std::vector<EncodedTile>& tiles,
    const DatasetId dataset_id) {
    Bytes bytes(tiles.size() * format_v1::bytes::tile_index_record);
    for (std::size_t index = 0; index < tiles.size(); ++index) {
        const EncodedTile& tile = tiles[index];
        const PackArtifact& pack = packs[index];
        const std::size_t offset = index * format_v1::bytes::tile_index_record;
        write_u64(bytes, offset + format_v1::tile_index_offset::tile_key, tile.key.encoded());
        write_u32(bytes, offset + format_v1::tile_index_offset::pack_id, pack.id.value);
        write_u32(bytes, offset + format_v1::tile_index_offset::flags, tile_has_provenance);
        write_u64(bytes, offset + format_v1::tile_index_offset::payload_offset, pack.payload_offset);
        write_u32(bytes, offset + format_v1::tile_index_offset::stored_bytes, pack.payload_bytes);
        write_u32(bytes, offset + format_v1::tile_index_offset::logical_bytes, tile.logical_channel_bytes);
        write_u16(bytes, offset + format_v1::tile_index_offset::minimum_elevation, tile.minimum_code);
        write_u16(bytes, offset + format_v1::tile_index_offset::maximum_elevation, tile.maximum_code);
        write_u32(bytes, offset + format_v1::tile_index_offset::primary_dataset, dataset_id.value);
        write_u32(bytes, offset + format_v1::tile_index_offset::effective_resolution, synthetic_resolution_meters * 1'000U);
        write_u32(bytes, offset + format_v1::tile_index_offset::geometric_error, 0);
        write_u8(bytes, offset + format_v1::tile_index_offset::channel_count, 2);
        write_u32(bytes, offset + format_v1::tile_index_offset::payload_crc32c, tile.payload_crc);
        write_bytes(bytes, offset + format_v1::tile_index_offset::content_hash, ByteView{tile.content_hash.bytes}.first<16>());
        write_bytes(bytes, offset + format_v1::tile_index_offset::dependency_hash, ByteView{tile.dependency_hash.bytes}.first<8>());
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
    const SyntheticDataset& dataset,
    const DatabaseId& database_id,
    const std::vector<EncodedTile>& tiles,
    const std::vector<PackArtifact>& packs) {
    auto strings = make_string_table(dataset, packs);
    if (!strings) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(strings).error());
    }
    const Bytes tidx = make_tile_index_chunk(packs, tiles, dataset.id);
    auto tidx_hash = framed_text_hash("LTDB_TILE_INDEX_V1", std::string_view{
        reinterpret_cast<const char*>(tidx.data()), tidx.size()});
    if (!tidx_hash) {
        return Result<std::pair<Bytes, Sha256Digest>>::failure(std::move(tidx_hash).error());
    }

    std::vector<ChunkArtifact> chunks{
        ChunkArtifact{{'D', 'S', 'E', 'T'}, make_dataset_chunk(dataset, strings.value()), 0},
        ChunkArtifact{{'M', 'E', 'T', 'A'}, Bytes{std::as_bytes(std::span{dataset.metadata_json}).begin(), std::as_bytes(std::span{dataset.metadata_json}).end()}, 0},
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
    write_u8(file, format_v1::ltdb_header_offset::maximum_level, 0);
    write_u8(file, format_v1::ltdb_header_offset::projection, static_cast<std::uint8_t>(ProjectionId::lunar_qsc_v1));
    write_u8(file, format_v1::ltdb_header_offset::quantization, static_cast<std::uint8_t>(QuantizationId::global_u16_0p5m));
    write_u64(file, format_v1::ltdb_header_offset::tile_count, tiles.size());
    write_u32(file, format_v1::ltdb_header_offset::dataset_count, 1);
    write_u32(file, format_v1::ltdb_header_offset::pack_count, static_cast<std::uint32_t>(packs.size()));
    write_bytes(file, format_v1::ltdb_header_offset::builder_configuration_hash, identity.builder_hash.bytes);
    write_bytes(file, format_v1::ltdb_header_offset::dataset_registry_hash, dataset.registry_hash.bytes);
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

}  // namespace

Result<BuildReport> build_synthetic(const BuilderConfiguration& configuration) {
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<BuildReport>::failure(std::move(identity).error());
    }
    auto dataset = make_dataset(configuration, identity.value());
    if (!dataset) {
        return Result<BuildReport>::failure(std::move(dataset).error());
    }
    auto database_id = make_database_id(identity.value().builder_hash, dataset.value().registry_hash);
    if (!database_id) {
        return Result<BuildReport>::failure(std::move(database_id).error());
    }
    auto tiles = build_tiles(configuration, identity.value(), dataset.value());
    if (!tiles) {
        return Result<BuildReport>::failure(std::move(tiles).error());
    }
    auto packs = build_packs(configuration, database_id.value(), tiles.value());
    if (!packs) {
        return Result<BuildReport>::failure(std::move(packs).error());
    }
    auto database = make_database_file(
        identity.value(), dataset.value(), database_id.value(), tiles.value(), packs.value());
    if (!database) {
        return Result<BuildReport>::failure(std::move(database).error());
    }

    const std::filesystem::path database_path =
        configuration.output_directory / (configuration.database_name + ".ltdb");
    auto published = publish_outputs(database_path, database.value().first, packs.value());
    if (!published) {
        return Result<BuildReport>::failure(std::move(published).error());
    }

    BuildReport report;
    report.database_path = database_path;
    report.database_content_hash = database.value().second;
    report.builder_configuration_hash = identity.value().builder_hash;
    report.tile_count = tiles.value().size();
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

}  // namespace lunar::terrain::builder
