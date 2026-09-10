#include "builder/raster_source.hpp"

#include "builder/fusion.hpp"
#include "builder/task_executor.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <cpl_error.h>
#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <openssl/evp.h>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/integrity.hpp>
#include <lunar/terrain/qsc_projection.hpp>

namespace lunar::terrain::builder {
namespace {

using Bytes = std::vector<std::byte>;

constexpr double radians_to_degrees = 180.0 / std::numbers::pi_v<double>;
constexpr double canonical_nan = std::bit_cast<double>(std::uint64_t{0x7FF8000000000000ULL});

struct DatasetDeleter {
    void operator()(GDALDataset* dataset) const noexcept {
        if (dataset != nullptr) {
            GDALClose(dataset);
        }
    }
};

struct CoordinateTransformationDeleter {
    void operator()(OGRCoordinateTransformation* transformation) const noexcept {
        if (transformation != nullptr) {
            OGRCoordinateTransformation::DestroyCT(transformation);
        }
    }
};

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept {
        EVP_MD_CTX_free(context);
    }
};

using DatasetPtr = std::unique_ptr<GDALDataset, DatasetDeleter>;
using CoordinateTransformationPtr =
    std::unique_ptr<OGRCoordinateTransformation, CoordinateTransformationDeleter>;

struct QualityRaster {
    DatasetPtr dataset;
    GDALRasterBand* band{};
    std::filesystem::path path;
    mutable bool cache_attempted{};
    mutable std::vector<std::uint8_t> cache_u8;
    mutable std::vector<float> cache_f32;
    std::optional<Sha256Digest> artifact_hash;
    std::filesystem::path decoded_cache_path;
};

[[nodiscard]] Error raster_error(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path = {}) {
    Error error{code, std::move(message)};
    if (!path.empty()) {
        error.with_path(path.string());
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path = {}) {
    return Result<T>::failure(raster_error(code, std::move(message), path));
}

[[nodiscard]] Result<void> check_telemetry(TelemetryCollector* const collector) {
    return collector == nullptr ? Result<void>::success() : collector->Check();
}

int gdal_progress(
    const double,
    const char*,
    void* const progress_data) {
    auto* const collector = static_cast<TelemetryCollector*>(progress_data);
    return collector == nullptr || static_cast<bool>(collector->Check());
}

[[nodiscard]] GDALRasterIOExtraArg raster_io_options(
    TelemetryCollector* const collector) {
    GDALRasterIOExtraArg options;
    INIT_RASTERIO_EXTRA_ARG(options);
    options.pfnProgress = gdal_progress;
    options.pProgressData = collector;
    return options;
}

void append_u8(Bytes& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void append_u32(Bytes& bytes, const std::uint32_t value) {
    for (std::uint32_t index = 0; index < 4U; ++index) {
        bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
    }
}

void append_u64(Bytes& bytes, const std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8U; ++index) {
        bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
    }
}

void append_f64(Bytes& bytes, const double value) {
    append_u64(bytes, std::bit_cast<std::uint64_t>(value));
}

void append_text(Bytes& bytes, const std::string_view value) {
    const auto view = std::as_bytes(std::span{value});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

void append_domain(Bytes& bytes, const std::string_view value) {
    append_text(bytes, value);
    append_u8(bytes, 0);
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

[[nodiscard]] Result<Sha256Digest> hash_file(
    const std::filesystem::path& path,
    TelemetryCollector* const collector) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open()) {
        return failure<Sha256Digest>(ErrorCode::io_error, "could not open raster artifact member", path);
    }
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return failure<Sha256Digest>(ErrorCode::internal_error, "could not initialize SHA-256", path);
    }
    std::vector<char> buffer(1U << 20U);
    while (stream) {
        auto checked = check_telemetry(collector);
        if (!checked) {
            return Result<Sha256Digest>::failure(std::move(checked).error());
        }
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = stream.gcount();
        if (bytes > 0 && EVP_DigestUpdate(
                context.get(), buffer.data(), static_cast<std::size_t>(bytes)) != 1) {
            return failure<Sha256Digest>(ErrorCode::internal_error, "could not update SHA-256", path);
        }
        if (bytes > 0 && collector != nullptr) {
            collector->AddIo(static_cast<std::uint64_t>(bytes), 0);
            collector->AddCount(
                "source_hash_bytes", static_cast<std::uint64_t>(bytes));
        }
    }
    if (!stream.eof()) {
        return failure<Sha256Digest>(ErrorCode::io_error, "could not read raster artifact member", path);
    }
    Sha256Digest digest;
    unsigned int digest_bytes = 0;
    if (EVP_DigestFinal_ex(
            context.get(), reinterpret_cast<unsigned char*>(digest.bytes.data()), &digest_bytes) != 1 ||
        digest_bytes != digest.bytes.size()) {
        return failure<Sha256Digest>(ErrorCode::internal_error, "could not finalize SHA-256", path);
    }
    return Result<Sha256Digest>::success(digest);
}

[[nodiscard]] std::uint16_t read_cache_u16(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept;
[[nodiscard]] std::uint32_t read_cache_u32(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept;
[[nodiscard]] std::uint64_t read_cache_u64(
    std::span<const std::byte> bytes,
    std::size_t offset) noexcept;

template <typename T>
[[nodiscard]] constexpr std::uint8_t decoded_cache_type() noexcept {
    if constexpr (std::is_same_v<T, std::int16_t>) {
        return 1U;
    } else if constexpr (std::is_same_v<T, float>) {
        return 2U;
    } else {
        static_assert(std::is_same_v<T, std::uint8_t>);
        return 3U;
    }
}

template <typename T>
void append_decoded_cache_value(Bytes& bytes, const T value) {
    if constexpr (std::is_same_v<T, std::int16_t>) {
        const std::uint16_t bits = std::bit_cast<std::uint16_t>(value);
        bytes.push_back(static_cast<std::byte>(bits));
        bytes.push_back(static_cast<std::byte>(bits >> 8U));
    } else if constexpr (std::is_same_v<T, float>) {
        append_u32(bytes, std::bit_cast<std::uint32_t>(value));
    } else {
        static_assert(std::is_same_v<T, std::uint8_t>);
        append_u8(bytes, value);
    }
}

template <typename T>
[[nodiscard]] T decoded_cache_value(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    if constexpr (std::is_same_v<T, std::int16_t>) {
        return std::bit_cast<std::int16_t>(read_cache_u16(bytes, offset));
    } else if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(read_cache_u32(bytes, offset));
    } else {
        static_assert(std::is_same_v<T, std::uint8_t>);
        return std::to_integer<std::uint8_t>(bytes[offset]);
    }
}

template <typename T, typename Consumer>
[[nodiscard]] Result<void> for_each_decoded_cache_chunk(
    const std::span<const T> samples,
    const std::filesystem::path& path,
    Consumer&& consumer,
    TelemetryCollector* const collector) {
    constexpr std::size_t target_chunk_bytes = 1U << 20U;
    const std::size_t values_per_chunk = std::max<std::size_t>(
        1U, target_chunk_bytes / sizeof(T));
    Bytes bytes;
    bytes.reserve(values_per_chunk * sizeof(T));
    for (std::size_t first = 0; first < samples.size(); first += values_per_chunk) {
        auto checked = check_telemetry(collector);
        if (!checked) {
            return checked;
        }
        bytes.clear();
        const std::size_t last = std::min(samples.size(), first + values_per_chunk);
        const std::size_t chunk_bytes = (last - first) * sizeof(T);
        if constexpr (std::endian::native == std::endian::little) {
            bytes.resize(chunk_bytes);
            std::memcpy(bytes.data(), samples.data() + first, chunk_bytes);
        } else {
            for (std::size_t index = first; index < last; ++index) {
                append_decoded_cache_value(bytes, samples[index]);
            }
        }
        if (!consumer(bytes)) {
            return failure<void>(
                ErrorCode::io_error,
                "could not process a decoded-raster cache chunk",
                path);
        }
    }
    return Result<void>::success();
}

template <typename T>
[[nodiscard]] Result<Sha256Digest> decoded_cache_payload_hash(
    const std::span<const T> samples,
    const std::filesystem::path& path,
    TelemetryCollector* const collector) {
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return failure<Sha256Digest>(
            ErrorCode::internal_error,
            "could not initialize decoded-raster cache SHA-256",
            path);
    }
    auto hashed = for_each_decoded_cache_chunk(
        samples,
        path,
        [&](const Bytes& bytes) {
            return EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) == 1;
        },
        collector);
    if (!hashed) {
        return Result<Sha256Digest>::failure(std::move(hashed).error());
    }
    Sha256Digest digest;
    unsigned int digest_bytes = 0;
    if (EVP_DigestFinal_ex(
            context.get(),
            reinterpret_cast<unsigned char*>(digest.bytes.data()),
            &digest_bytes) != 1 ||
        digest_bytes != digest.bytes.size()) {
        return failure<Sha256Digest>(
            ErrorCode::internal_error,
            "could not finalize decoded-raster cache SHA-256",
            path);
    }
    return Result<Sha256Digest>::success(digest);
}

[[nodiscard]] std::filesystem::path decoded_cache_path(
    const std::filesystem::path& root,
    const Sha256Digest& artifact_hash,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::uint8_t type) {
    return root / fmt::format(
        "{}-{}x{}-t{}.decoded-v1",
        artifact_hash.to_hex(),
        width,
        height,
        type);
}

template <typename T>
[[nodiscard]] Result<void> persist_decoded_cache(
    const std::filesystem::path& path,
    const Sha256Digest& artifact_hash,
    const std::uint32_t width,
    const std::uint32_t height,
    const std::span<const T> samples,
    TelemetryCollector* const collector) {
    constexpr std::size_t header_bytes = 88U;
    const std::uint64_t expected_samples = std::uint64_t{width} * height;
    if (samples.size() != expected_samples ||
        samples.size() > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) {
        return failure<void>(
            samples.size() != expected_samples
                ? ErrorCode::invalid_argument
                : ErrorCode::arithmetic_overflow,
            samples.size() != expected_samples
                ? "decoded-raster cache dimensions do not match its samples"
                : "decoded-raster cache payload byte count overflows",
            path);
    }
    const std::uint64_t payload_bytes =
        static_cast<std::uint64_t>(samples.size()) * sizeof(T);
    auto payload_hash = decoded_cache_payload_hash(samples, path, collector);
    if (!payload_hash) {
        return Result<void>::failure(std::move(payload_hash).error());
    }
    Bytes header;
    header.reserve(header_bytes);
    append_text(header, "LTDR");
    header.push_back(std::byte{1});
    header.push_back(std::byte{0});
    append_u8(header, decoded_cache_type<T>());
    append_u8(header, 0);
    append_u32(header, width);
    append_u32(header, height);
    append_u64(header, payload_bytes);
    header.insert(header.end(), artifact_hash.bytes.begin(), artifact_hash.bytes.end());
    header.insert(
        header.end(), payload_hash.value().bytes.begin(), payload_hash.value().bytes.end());
    if (header.size() != header_bytes) {
        return failure<void>(
            ErrorCode::internal_error,
            "decoded-raster cache header size is inconsistent",
            path);
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(path.parent_path(), filesystem_error);
    if (filesystem_error) {
        return failure<void>(
            ErrorCode::io_error,
            fmt::format(
                "could not create decoded-raster cache directory: {}",
                filesystem_error.message()),
            path.parent_path());
    }
    const std::filesystem::path temporary =
        path.parent_path() / ("." + path.filename().string() + ".tmp");
    std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return failure<void>(
            ErrorCode::io_error,
            "could not create decoded-raster cache",
            temporary);
    }
    stream.write(
        reinterpret_cast<const char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    auto written = for_each_decoded_cache_chunk(
        samples,
        temporary,
        [&](const Bytes& bytes) {
            stream.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
            if (stream && collector != nullptr) {
                collector->AddIo(0, bytes.size());
                collector->AddCount("decoded_cache_write_bytes", bytes.size());
            }
            return static_cast<bool>(stream);
        },
        collector);
    stream.flush();
    stream.close();
    if (!written || !stream) {
        std::filesystem::remove(temporary, filesystem_error);
        return written
            ? failure<void>(
                  ErrorCode::io_error,
                  "could not write decoded-raster cache",
                  temporary)
            : written;
    }
    std::filesystem::rename(temporary, path, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(temporary, filesystem_error);
        return failure<void>(
            ErrorCode::io_error,
            fmt::format(
                "could not publish decoded-raster cache: {}",
                filesystem_error.message()),
            path);
    }
    return Result<void>::success();
}

template <typename T>
[[nodiscard]] Result<void> load_decoded_cache(
    const std::filesystem::path& path,
    const Sha256Digest& expected_artifact_hash,
    const std::uint32_t expected_width,
    const std::uint32_t expected_height,
    std::vector<T>& samples,
    TelemetryCollector* const collector) {
    constexpr std::size_t header_bytes = 88U;
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return failure<void>(
            ErrorCode::io_error,
            "could not open decoded-raster cache",
            path);
    }
    const std::streamoff file_bytes = stream.tellg();
    if (expected_height != 0 &&
        expected_width > std::numeric_limits<std::uint64_t>::max() / expected_height) {
        return failure<void>(
            ErrorCode::arithmetic_overflow,
            "decoded-raster cache sample count overflows",
            path);
    }
    const std::uint64_t expected_samples =
        std::uint64_t{expected_width} * expected_height;
    if (expected_samples > std::numeric_limits<std::size_t>::max() ||
        expected_samples > std::numeric_limits<std::uint64_t>::max() / sizeof(T)) {
        return failure<void>(
            ErrorCode::arithmetic_overflow,
            "decoded-raster cache payload exceeds hosted limits",
            path);
    }
    const std::uint64_t expected_payload_bytes = expected_samples * sizeof(T);
    if (expected_payload_bytes >
            std::numeric_limits<std::uint64_t>::max() - header_bytes ||
        file_bytes < 0 ||
        static_cast<std::uint64_t>(file_bytes) != header_bytes + expected_payload_bytes) {
        return failure<void>(
            ErrorCode::invalid_format,
            "decoded-raster cache byte count is invalid",
            path);
    }
    stream.seekg(0, std::ios::beg);
    std::array<std::byte, header_bytes> header{};
    stream.read(
        reinterpret_cast<char*>(header.data()),
        static_cast<std::streamsize>(header.size()));
    const std::span<const std::byte> header_view = header;
    if (!stream ||
        std::string_view{reinterpret_cast<const char*>(header.data()), 4} != "LTDR" ||
        std::to_integer<std::uint8_t>(header[4]) != 1 || header[5] != std::byte{0} ||
        std::to_integer<std::uint8_t>(header[6]) != decoded_cache_type<T>() ||
        header[7] != std::byte{0} || read_cache_u32(header_view, 8) != expected_width ||
        read_cache_u32(header_view, 12) != expected_height ||
        read_cache_u64(header_view, 16) != expected_payload_bytes ||
        !std::ranges::equal(
            header_view.subspan(24, expected_artifact_hash.bytes.size()),
            expected_artifact_hash.bytes)) {
        return failure<void>(
            ErrorCode::hash_mismatch,
            "decoded-raster cache identity does not match its artifact",
            path);
    }
    Sha256Digest expected_payload_hash;
    std::ranges::copy(header_view.subspan(56, 32), expected_payload_hash.bytes.begin());
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context{EVP_MD_CTX_new()};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return failure<void>(
            ErrorCode::internal_error,
            "could not initialize decoded-raster cache verification",
            path);
    }
    samples.resize(static_cast<std::size_t>(expected_samples));
    constexpr std::size_t target_chunk_bytes = 1U << 20U;
    Bytes bytes(target_chunk_bytes);
    std::size_t sample_index = 0;
    while (sample_index < samples.size()) {
        auto checked = check_telemetry(collector);
        if (!checked) {
            samples.clear();
            return checked;
        }
        const std::size_t chunk_samples = std::min<std::size_t>(
            samples.size() - sample_index,
            target_chunk_bytes / sizeof(T));
        const std::size_t chunk_bytes = chunk_samples * sizeof(T);
        stream.read(
            reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(chunk_bytes));
        if (!stream || EVP_DigestUpdate(context.get(), bytes.data(), chunk_bytes) != 1) {
            samples.clear();
            return failure<void>(
                ErrorCode::io_error,
                "could not read decoded-raster cache payload",
                path);
        }
        if (collector != nullptr) {
            collector->AddIo(chunk_bytes, 0);
            collector->AddCount("decoded_cache_read_bytes", chunk_bytes);
        }
        const std::span<const std::byte> chunk{bytes.data(), chunk_bytes};
        if constexpr (std::endian::native == std::endian::little) {
            std::memcpy(samples.data() + sample_index, bytes.data(), chunk_bytes);
        } else {
            for (std::size_t index = 0; index < chunk_samples; ++index) {
                samples[sample_index + index] = decoded_cache_value<T>(
                    chunk, index * sizeof(T));
            }
        }
        sample_index += chunk_samples;
    }
    Sha256Digest payload_hash;
    unsigned int digest_bytes = 0;
    if (EVP_DigestFinal_ex(
            context.get(),
            reinterpret_cast<unsigned char*>(payload_hash.bytes.data()),
            &digest_bytes) != 1 ||
        digest_bytes != payload_hash.bytes.size() || payload_hash != expected_payload_hash) {
        samples.clear();
        return failure<void>(
            ErrorCode::checksum_mismatch,
            "decoded-raster cache payload SHA-256 does not match",
            path);
    }
    return Result<void>::success();
}

[[nodiscard]] Result<std::vector<ArtifactMember>> catalog_artifacts(
    const RasterConfiguration& configuration,
    TelemetryCollector* const collector) {
    std::vector<ArtifactMember> members;
    members.reserve(configuration.artifact_members.size());
    for (const ArtifactMemberConfiguration& expected : configuration.artifact_members) {
        auto checked = check_telemetry(collector);
        if (!checked) {
            return Result<std::vector<ArtifactMember>>::failure(
                std::move(checked).error());
        }
        const std::filesystem::path path = configuration.source_root /
            std::filesystem::path{expected.name};
        std::error_code filesystem_error;
        const std::uint64_t bytes = std::filesystem::file_size(path, filesystem_error);
        if (filesystem_error) {
            return failure<std::vector<ArtifactMember>>(
                ErrorCode::io_error,
                fmt::format("could not inspect raster artifact member: {}", filesystem_error.message()),
                path);
        }
        if (expected.expected_bytes && bytes != *expected.expected_bytes) {
            return failure<std::vector<ArtifactMember>>(
                ErrorCode::hash_mismatch,
                fmt::format("raster artifact byte count is {}, expected {}", bytes, *expected.expected_bytes),
                path);
        }
        auto digest = hash_file(path, collector);
        if (!digest) {
            return Result<std::vector<ArtifactMember>>::failure(std::move(digest).error());
        }
        if (expected.expected_sha256 && digest.value() != *expected.expected_sha256) {
            return failure<std::vector<ArtifactMember>>(
                ErrorCode::hash_mismatch, "raster artifact SHA-256 does not match configuration", path);
        }
        members.push_back(ArtifactMember{expected.name, bytes, digest.value()});
    }
    return Result<std::vector<ArtifactMember>>::success(std::move(members));
}

[[nodiscard]] Result<std::pair<std::uint64_t, Sha256Digest>> artifact_bundle_identity(
    const std::vector<ArtifactMember>& members,
    const RasterConfiguration& configuration) {
    Bytes input;
    append_domain(input, "LTDB_ARTIFACT_BUNDLE_V1");
    append_u32(input, static_cast<std::uint32_t>(members.size()));
    std::uint64_t total = 0;
    for (const ArtifactMember& member : members) {
        if (member.bytes > std::numeric_limits<std::uint64_t>::max() - total) {
            return failure<std::pair<std::uint64_t, Sha256Digest>>(
                ErrorCode::arithmetic_overflow, "raster artifact bundle byte count overflows");
        }
        total += member.bytes;
        append_u32(input, static_cast<std::uint32_t>(member.name.size()));
        append_text(input, member.name);
        append_u64(input, member.bytes);
        input.insert(input.end(), member.sha256.bytes.begin(), member.sha256.bytes.end());
    }
    auto digest = sha256(input);
    if (!digest) {
        return Result<std::pair<std::uint64_t, Sha256Digest>>::failure(std::move(digest).error());
    }
    if (configuration.expected_bundle_bytes && total != *configuration.expected_bundle_bytes) {
        return failure<std::pair<std::uint64_t, Sha256Digest>>(
            ErrorCode::hash_mismatch,
            fmt::format("raster artifact bundle contains {} bytes, expected {}",
                        total, *configuration.expected_bundle_bytes));
    }
    if (configuration.expected_bundle_sha256 && digest.value() != *configuration.expected_bundle_sha256) {
        return failure<std::pair<std::uint64_t, Sha256Digest>>(
            ErrorCode::hash_mismatch, "raster artifact bundle SHA-256 does not match configuration");
    }
    return Result<std::pair<std::uint64_t, Sha256Digest>>::success(
        std::pair{total, digest.value()});
}

[[nodiscard]] std::string artifact_members_json(const std::vector<ArtifactMember>& members) {
    std::string result;
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += fmt::format(
            "{{\"bytes\":{},\"name\":{},\"sha256\":{}}}",
            members[index].bytes,
            json_string(members[index].name),
            json_string(members[index].sha256.to_hex()));
    }
    return result;
}

[[nodiscard]] std::uint16_t read_cache_u16(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_cache_u32(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4U; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_cache_u64(
    const std::span<const std::byte> bytes,
    const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::uint32_t index = 0; index < 8U; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::string metadata_double_json(const double value) {
    return std::isnan(value) ? json_string("nan") : fmt::format("{}", value);
}

[[nodiscard]] std::string metadata_strings_json(const std::vector<std::string>& values) {
    std::string result;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += json_string(values[index]);
    }
    return result;
}

[[nodiscard]] std::string metadata_raster_files_json(
    const std::vector<RasterFileConfiguration>& files) {
    std::string result;
    for (std::size_t index = 0; index < files.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        const RasterFileConfiguration& file = files[index];
        result += fmt::format(
            "{{\"east_longitude_degrees\":{},\"expected_height\":{},"
            "\"expected_width\":{},\"member\":{},\"north_latitude_degrees\":{},"
            "\"south_latitude_degrees\":{},\"west_longitude_degrees\":{}}}",
            file.bounds.east_longitude_degrees,
            file.expected_height,
            file.expected_width,
            json_string(file.member),
            file.bounds.north_latitude_degrees,
            file.bounds.south_latitude_degrees,
            file.bounds.west_longitude_degrees);
    }
    return result;
}

[[nodiscard]] Result<DatasetArtifact> make_dataset_artifact(
    const RasterConfiguration& configuration,
    const DatasetId dataset_id,
    std::vector<ArtifactMember> members,
    const std::uint64_t bundle_bytes,
    const Sha256Digest bundle_hash) {
    DatasetArtifact dataset;
    dataset.id = dataset_id;
    dataset.flags = 0x00000002U;
    if (configuration.elevation_representation == ElevationRepresentation::radius_meters) {
        dataset.flags |= 0x00000001U;
    }
    dataset.strings = {
        configuration.product_name,
        configuration.producer,
        configuration.mission,
        configuration.instrument,
        configuration.product_version,
        configuration.source_uri,
        configuration.original_crs,
        configuration.license,
    };
    dataset.nominal_resolution_meters = configuration.nominal_resolution_meters;
    dataset.effective_resolution_meters = configuration.effective_resolution_meters;
    dataset.horizontal_accuracy_meters = canonical_nan;
    dataset.vertical_accuracy_meters = canonical_nan;
    dataset.source_no_data = configuration.source_no_data;
    dataset.artifact_members = std::move(members);
    dataset.artifact_bundle_bytes = bundle_bytes;
    dataset.artifact_bundle_hash = bundle_hash;
    dataset.datum_version = "lunar_radial_1737400_v1";
    dataset.sampling_algorithm = "gdal_bilinear_v1";
    const std::string representation = configuration.elevation_representation ==
        ElevationRepresentation::elevation_meters ? "elevation_meters" : "radius_meters";
    const std::string policy = configuration.no_data_policy == NoDataPolicy::error
        ? "error" : "nearest_valid";
    const std::string metadata_overrides = configuration.metadata_override
        ? "{\"georeferencing\":\"configured_bounds\",\"no_data\":\"configuration\","
          "\"sample_offset\":\"configuration\",\"sample_scale\":\"configuration\"}"
        : "{}";
    const std::string_view fusion_policy = [&configuration]() -> std::string_view {
        switch (configuration.fusion_policy) {
            case FusionPolicy::replace:
                return "Replace";
            case FusionPolicy::bias_corrected_replace:
                return "BiasCorrectedReplace";
            case FusionPolicy::residual_refinement_v1:
                return "ResidualRefinement_v1";
        }
        return "Replace";
    }();
    dataset.metadata_json = fmt::format(
        "{{\"artifact_members\":[{}],\"auxiliary_member\":{},\"data_type\":{},"
        "\"datum\":{{\"reference_radius_m\":1737400,\"source_reference_radius_m\":{}}},"
        "\"effective_resolution_meters\":{},\"elevation_representation\":{},"
        "\"footprint\":{{\"east_longitude_degrees\":{},"
        "\"north_latitude_degrees\":{},\"south_latitude_degrees\":{},"
        "\"west_longitude_degrees\":{}}},\"fusion_policy\":{},"
        "\"label_member\":{},\"metadata_overrides\":{},"
        "\"no_data\":{{\"policy\":{},\"sample_offset\":{},\"sample_scale\":{},\"value\":{}}},"
        "\"priority\":{},\"raster_member\":{},\"stable_key\":{}}}",
        artifact_members_json(dataset.artifact_members),
        json_string(configuration.auxiliary_member),
        json_string(configuration.expected_data_type),
        configuration.source_reference_radius_meters,
        configuration.effective_resolution_meters,
        json_string(representation),
        configuration.east_longitude_degrees,
        configuration.north_latitude_degrees,
        configuration.south_latitude_degrees,
        configuration.west_longitude_degrees,
        json_string(fusion_policy),
        json_string(configuration.label_member),
        metadata_overrides,
        json_string(policy),
        configuration.sample_offset,
        configuration.sample_scale,
        metadata_double_json(configuration.source_no_data),
        configuration.priority,
        json_string(configuration.raster_member),
        json_string(configuration.stable_key));
    if (configuration.raster_files.size() > 1U || !configuration.quality_mapping.empty() ||
        !configuration.quality_members.empty() ||
        !configuration.unsupported_quality_values.empty()) {
        dataset.metadata_json.pop_back();
        dataset.metadata_json += fmt::format(
            ",\"quality_mapping\":{},\"quality_members\":[{}],\"raster_files\":[{}],"
            "\"unsupported_quality_values\":[{}]}}",
            json_string(configuration.quality_mapping),
            metadata_strings_json(configuration.quality_members),
            metadata_raster_files_json(configuration.raster_files),
            metadata_strings_json(configuration.unsupported_quality_values));
    }

    Bytes registry_input;
    append_domain(registry_input, "LTDB_DATASET_REGISTRY_V1");
    append_u32(registry_input, 1);
    append_u32(registry_input, dataset.id.value);
    append_u32(registry_input, dataset.flags);
    for (const std::string& text : dataset.strings) {
        append_u64(registry_input, text.size());
        append_text(registry_input, text);
    }
    append_f64(registry_input, dataset.nominal_resolution_meters);
    append_f64(registry_input, dataset.horizontal_accuracy_meters);
    append_f64(registry_input, dataset.vertical_accuracy_meters);
    append_f64(registry_input, dataset.source_no_data);
    append_u64(registry_input, dataset.artifact_bundle_bytes);
    registry_input.insert(
        registry_input.end(), dataset.artifact_bundle_hash.bytes.begin(),
        dataset.artifact_bundle_hash.bytes.end());
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

void register_gdal() {
    static std::once_flag registered;
    std::call_once(registered, [] { GDALAllRegister(); });
}

class GdalErrorHandlerScope {
public:
    explicit GdalErrorHandlerScope(const bool quiet) : quiet_(quiet) {
        if (quiet_) {
            CPLPushErrorHandler(CPLQuietErrorHandler);
        }
    }

    ~GdalErrorHandlerScope() {
        if (quiet_) {
            CPLPopErrorHandler();
        }
    }

    GdalErrorHandlerScope(const GdalErrorHandlerScope&) = delete;
    GdalErrorHandlerScope& operator=(const GdalErrorHandlerScope&) = delete;

private:
    bool quiet_{};
};

[[nodiscard]] bool almost_equal(const double left, const double right) noexcept {
    const double scale = std::max({1.0, std::abs(left), std::abs(right)});
    return std::abs(left - right) <= scale * 1.0e-10;
}

[[nodiscard]] bool almost_equal_degrees(const double left, const double right) noexcept {
    return std::abs(left - right) <= 1.0e-7;
}

[[nodiscard]] std::optional<Sha256Digest> artifact_member_hash(
    const DatasetArtifact& dataset,
    const std::string_view member) {
    const auto found = std::ranges::find(
        dataset.artifact_members, member, &ArtifactMember::name);
    return found == dataset.artifact_members.end()
        ? std::nullopt
        : std::optional{found->sha256};
}

class GdalRasterSource final : public IRasterSource {
public:
    static Result<std::unique_ptr<IRasterSource>> Open(
        const RasterConfiguration& configuration,
        DatasetArtifact dataset,
        const std::optional<std::filesystem::path>& decoded_cache_root,
        TelemetryCollector* const collector) {
        register_gdal();
        const std::filesystem::path raster_path = configuration.source_root /
            std::filesystem::path{configuration.raster_member};
        if (raster_path.extension() == ".jp2" || raster_path.extension() == ".JP2") {
            if (GetGDALDriverManager()->GetDriverByName("JP2OpenJPEG") == nullptr) {
                return failure<std::unique_ptr<IRasterSource>>(
                    ErrorCode::unsupported_feature,
                    "the linked GDAL runtime does not expose the JP2OpenJPEG driver",
                    raster_path);
            }
        }
        DatasetPtr opened;
        {
            GdalErrorHandlerScope errors{configuration.metadata_override};
            opened.reset(static_cast<GDALDataset*>(GDALOpenEx(
                raster_path.string().c_str(), GDAL_OF_RASTER | GDAL_OF_READONLY,
                nullptr, nullptr, nullptr)));
        }
        if (!opened) {
            return failure<std::unique_ptr<IRasterSource>>(
                ErrorCode::io_error,
                fmt::format("GDAL could not open the raster: {}", CPLGetLastErrorMsg()),
                raster_path);
        }
        if (opened->GetRasterXSize() <= 0 || opened->GetRasterYSize() <= 0 ||
            opened->GetRasterCount() < 1) {
            return failure<std::unique_ptr<IRasterSource>>(
                ErrorCode::invalid_argument, "GDAL raster dimensions or band count are invalid", raster_path);
        }
        if (static_cast<std::uint32_t>(opened->GetRasterXSize()) != configuration.expected_width ||
            static_cast<std::uint32_t>(opened->GetRasterYSize()) != configuration.expected_height) {
            return failure<std::unique_ptr<IRasterSource>>(
                ErrorCode::invalid_argument,
                fmt::format("GDAL raster dimensions are {}x{}, expected {}x{}",
                            opened->GetRasterXSize(), opened->GetRasterYSize(),
                            configuration.expected_width, configuration.expected_height),
                raster_path);
        }
        const std::string driver_name = opened->GetDriver() == nullptr
            ? std::string{} : std::string{opened->GetDriver()->GetDescription()};
        if ((raster_path.extension() == ".jp2" || raster_path.extension() == ".JP2") &&
            driver_name != "JP2OpenJPEG") {
            return failure<std::unique_ptr<IRasterSource>>(
                ErrorCode::unsupported_feature,
                fmt::format("JP2 source opened through '{}', not JP2OpenJPEG", driver_name),
                raster_path);
        }

        std::vector<QualityRaster> quality_rasters;
        quality_rasters.reserve(configuration.quality_members.size());
        for (const std::string& member : configuration.quality_members) {
            const std::filesystem::path quality_path =
                configuration.source_root / std::filesystem::path{member};
            DatasetPtr quality_dataset{static_cast<GDALDataset*>(GDALOpenEx(
                quality_path.string().c_str(),
                GDAL_OF_RASTER | GDAL_OF_READONLY,
                nullptr,
                nullptr,
                nullptr))};
            if (!quality_dataset || quality_dataset->GetRasterCount() < 1) {
                return failure<std::unique_ptr<IRasterSource>>(
                    ErrorCode::io_error,
                    fmt::format("GDAL could not open the quality companion: {}", CPLGetLastErrorMsg()),
                    quality_path);
            }
            if (quality_dataset->GetRasterXSize() != opened->GetRasterXSize() ||
                quality_dataset->GetRasterYSize() != opened->GetRasterYSize()) {
                return failure<std::unique_ptr<IRasterSource>>(
                    ErrorCode::invalid_argument,
                    "quality companion dimensions do not match the elevation raster",
                    quality_path);
            }
            const std::optional<Sha256Digest> quality_hash =
                artifact_member_hash(dataset, member);
            const GDALDataType quality_type =
                quality_dataset->GetRasterBand(1)->GetRasterDataType();
            const std::filesystem::path quality_cache =
                decoded_cache_root && quality_hash
                ? decoded_cache_path(
                      *decoded_cache_root,
                      *quality_hash,
                      configuration.expected_width,
                      configuration.expected_height,
                      quality_type == GDT_Byte
                          ? decoded_cache_type<std::uint8_t>()
                          : decoded_cache_type<float>())
                : std::filesystem::path{};
            quality_rasters.push_back(QualityRaster{
                std::move(quality_dataset),
                nullptr,
                quality_path,
                false,
                {},
                {},
                quality_hash,
                quality_cache});
            quality_rasters.back().band = quality_rasters.back().dataset->GetRasterBand(1);
        }

        const std::optional<Sha256Digest> raster_hash =
            artifact_member_hash(dataset, configuration.raster_member);
        std::filesystem::path raster_cache;
        if (decoded_cache_root && raster_hash) {
            const GDALDataType data_type = opened->GetRasterBand(1)->GetRasterDataType();
            if (data_type == GDT_Int16) {
                raster_cache = decoded_cache_path(
                    *decoded_cache_root,
                    *raster_hash,
                    configuration.expected_width,
                    configuration.expected_height,
                    decoded_cache_type<std::int16_t>());
            } else if (data_type == GDT_Float32) {
                raster_cache = decoded_cache_path(
                    *decoded_cache_root,
                    *raster_hash,
                    configuration.expected_width,
                    configuration.expected_height,
                    decoded_cache_type<float>());
            }
        }

        auto source = std::unique_ptr<GdalRasterSource>{new GdalRasterSource(
            configuration,
            std::move(dataset),
            std::move(opened),
            raster_path,
            driver_name,
            std::move(quality_rasters),
            raster_hash,
            std::move(raster_cache),
            collector)};
        auto initialized = source->InitializeGeoreferencing();
        if (!initialized) {
            return Result<std::unique_ptr<IRasterSource>>::failure(std::move(initialized).error());
        }
        auto metadata = source->ValidateBandMetadata();
        if (!metadata) {
            return Result<std::unique_ptr<IRasterSource>>::failure(std::move(metadata).error());
        }
        const bool quality_caches_available = std::ranges::all_of(
            source->quality_rasters_, [](const QualityRaster& quality) {
                return !quality.decoded_cache_path.empty() && quality.artifact_hash.has_value();
            });
        if (configuration.metadata_override &&
            !source->decoded_cache_path_.empty() &&
            source->raster_artifact_hash_ &&
            quality_caches_available) {
            auto cached = source->EnsureFullRasterCache();
            if (!cached) {
                return Result<std::unique_ptr<IRasterSource>>::failure(
                    std::move(cached).error());
            }
            cached = source->EnsureQualityCaches();
            if (!cached) {
                return Result<std::unique_ptr<IRasterSource>>::failure(
                    std::move(cached).error());
            }
            source->concurrent_cached_sampling_ = true;
        }
        return Result<std::unique_ptr<IRasterSource>>::success(std::move(source));
    }

    [[nodiscard]] const DatasetArtifact& metadata() const noexcept override { return dataset_; }
    [[nodiscard]] const RasterSourceDetails& details() const noexcept override { return details_; }

    [[nodiscard]] bool Covers(const LunarGeodeticCoordinate coordinate) const noexcept override {
        if (!std::isfinite(coordinate.latitude_radians) ||
            !std::isfinite(coordinate.longitude_radians)) {
            return false;
        }
        double longitude = coordinate.longitude_radians * radians_to_degrees;
        if (details_.footprint.west_longitude_degrees >= 0.0 && longitude < 0.0) {
            longitude += 360.0;
        }
        const double latitude = coordinate.latitude_radians * radians_to_degrees;
        return longitude >= details_.footprint.west_longitude_degrees &&
               longitude <= details_.footprint.east_longitude_degrees &&
               latitude >= details_.footprint.south_latitude_degrees &&
               latitude <= details_.footprint.north_latitude_degrees;
    }

    [[nodiscard]] Result<RawTerrainSample> Sample(
        const LunarGeodeticCoordinate coordinate) const override {
        const std::scoped_lock lock{mutex_};
        auto sample = SampleUnlocked(coordinate);
        if (!sample || quality_rasters_.empty()) {
            return sample;
        }
        auto pixel = PixelCoordinate(coordinate);
        if (!pixel) {
            return Result<RawTerrainSample>::failure(std::move(pixel).error());
        }
        auto accepted = ApplyQualitySample(sample.value(), pixel.value());
        if (!accepted) {
            return Result<RawTerrainSample>::failure(std::move(accepted).error());
        }
        if (!accepted.value()) {
            return failure<RawTerrainSample>(
                ErrorCode::invalid_argument,
                "quality companion marks the raster sample as no-data",
                raster_path_);
        }
        return sample;
    }

    [[nodiscard]] Result<std::vector<std::optional<RawTerrainSample>>> SampleBatch(
        const std::span<const LunarGeodeticCoordinate> coordinates,
        const std::stop_token cancellation) const override {
        if (concurrent_cached_sampling_) {
            return SampleBatchUnlocked(coordinates, cancellation);
        }
        const std::scoped_lock lock{mutex_};
        return SampleBatchUnlocked(coordinates, cancellation);
    }

private:
    [[nodiscard]] Result<std::vector<std::optional<RawTerrainSample>>> SampleBatchUnlocked(
        const std::span<const LunarGeodeticCoordinate> coordinates,
        const std::stop_token cancellation) const {
        std::vector<std::optional<std::array<double, 2>>> pixels(coordinates.size());
        int window_x0 = dataset_handle_->GetRasterXSize() - 1;
        int window_y0 = dataset_handle_->GetRasterYSize() - 1;
        int window_x1 = 0;
        int window_y1 = 0;
        bool any_covered = false;
        for (std::size_t index = 0; index < coordinates.size(); ++index) {
            if ((index % 256U) == 0U && cancellation.stop_requested()) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::cancelled, "raster sampling was cancelled", raster_path_);
            }
            const LunarGeodeticCoordinate coordinate = coordinates[index];
            if (!Covers(coordinate)) {
                continue;
            }
            auto pixel = PixelCoordinate(coordinate);
            if (!pixel) {
                return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                    std::move(pixel).error());
            }
            pixels[index] = pixel.value();
            const double centered_x = pixel.value()[0] - 0.5;
            const double centered_y = pixel.value()[1] - 0.5;
            const int maximum_x = dataset_handle_->GetRasterXSize() - 1;
            const int maximum_y = dataset_handle_->GetRasterYSize() - 1;
            const int x0 = std::clamp(static_cast<int>(std::floor(centered_x)), 0, maximum_x);
            const int y0 = std::clamp(static_cast<int>(std::floor(centered_y)), 0, maximum_y);
            window_x0 = std::min(window_x0, x0);
            window_y0 = std::min(window_y0, y0);
            window_x1 = std::max(window_x1, std::min(x0 + 1, maximum_x));
            window_y1 = std::max(window_y1, std::min(y0 + 1, maximum_y));
            any_covered = true;
        }
        std::vector<std::optional<RawTerrainSample>> samples(coordinates.size());
        if (!any_covered) {
            return Result<std::vector<std::optional<RawTerrainSample>>>::success(
                std::move(samples));
        }

        const int window_width = window_x1 - window_x0 + 1;
        const int window_height = window_y1 - window_y0 + 1;
        constexpr std::uint64_t maximum_bulk_samples = 4U * 1024U * 1024U;
        if (concurrent_cached_sampling_ ||
            std::uint64_t{static_cast<std::uint32_t>(window_width)} *
                    static_cast<std::uint32_t>(window_height) > maximum_bulk_samples) {
            auto cached = EnsureFullRasterCache();
            if (!cached) {
                return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                    std::move(cached).error());
            }
            int block_width = 0;
            int block_height = 0;
            band_->GetBlockSize(&block_width, &block_height);
            block_width = std::max(block_width, 1);
            block_height = std::max(block_height, 1);
            std::vector<std::size_t> sample_order;
            sample_order.reserve(coordinates.size());
            for (std::size_t index = 0; index < coordinates.size(); ++index) {
                if (pixels[index]) {
                    sample_order.push_back(index);
                }
            }
            std::ranges::sort(sample_order, {}, [&](const std::size_t index) {
                const int x = std::clamp(
                    static_cast<int>(std::floor((*pixels[index])[0] - 0.5)),
                    0,
                    dataset_handle_->GetRasterXSize() - 1);
                const int y = std::clamp(
                    static_cast<int>(std::floor((*pixels[index])[1] - 0.5)),
                    0,
                    dataset_handle_->GetRasterYSize() - 1);
                return std::tuple{y / block_height, x / block_width, y, x, index};
            });
            for (const std::size_t index : sample_order) {
                if (cancellation.stop_requested()) {
                    return failure<std::vector<std::optional<RawTerrainSample>>>(
                        ErrorCode::cancelled, "raster sampling was cancelled", raster_path_);
                }
                auto sample = SampleUnlocked(coordinates[index]);
                if (!sample) {
                    if (sample.error().code == ErrorCode::not_found ||
                        (sample.error().code == ErrorCode::invalid_argument &&
                         sample.error().message.find("no-data") != std::string::npos)) {
                        continue;
                    }
                    return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                        std::move(sample).error());
                }
                samples[index] = std::move(sample).value();
            }
            auto quality = ApplyQualityBatch(
                samples,
                pixels,
                window_x0,
                window_y0,
                window_x1,
                window_y1);
            if (!quality) {
                return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                    std::move(quality).error());
            }
            return Result<std::vector<std::optional<RawTerrainSample>>>::success(
                std::move(samples));
        }

        std::vector<double> values(
            static_cast<std::size_t>(window_width) * window_height);
        auto io_options = raster_io_options(telemetry_);
        if (band_->RasterIO(
                GF_Read,
                window_x0,
                window_y0,
                window_width,
                window_height,
                values.data(),
                window_width,
                window_height,
                GDT_Float64,
                0,
                0,
                &io_options) != CE_None) {
            if (cancellation.stop_requested()) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::cancelled, "raster sampling was cancelled", raster_path_);
            }
            auto checked = check_telemetry(telemetry_);
            if (!checked) {
                return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                    std::move(checked).error());
            }
            return failure<std::vector<std::optional<RawTerrainSample>>>(
                ErrorCode::io_error,
                fmt::format("GDAL batch sampling failed: {}", CPLGetLastErrorMsg()),
                raster_path_);
        }

        const int maximum_x = dataset_handle_->GetRasterXSize() - 1;
        const int maximum_y = dataset_handle_->GetRasterYSize() - 1;
        for (std::size_t index = 0; index < pixels.size(); ++index) {
            if ((index % 256U) == 0U && cancellation.stop_requested()) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::cancelled, "raster sampling was cancelled", raster_path_);
            }
            if (!pixels[index]) {
                continue;
            }
            const double centered_x = (*pixels[index])[0] - 0.5;
            const double centered_y = (*pixels[index])[1] - 0.5;
            const int x0 = std::clamp(static_cast<int>(std::floor(centered_x)), 0, maximum_x);
            const int y0 = std::clamp(static_cast<int>(std::floor(centered_y)), 0, maximum_y);
            const int x1 = std::min(x0 + 1, maximum_x);
            const int y1 = std::min(y0 + 1, maximum_y);
            const double fx = std::clamp(centered_x - static_cast<double>(x0), 0.0, 1.0);
            const double fy = std::clamp(centered_y - static_cast<double>(y0), 0.0, 1.0);
            const std::array weights{
                (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
                (1.0 - fx) * fy, fx * fy};
            const auto value_at = [&](const int x, const int y) {
                return values[static_cast<std::size_t>(y - window_y0) * window_width +
                              static_cast<std::size_t>(x - window_x0)];
            };
            const std::array sample_values{
                value_at(x0, y0), value_at(x1, y0), value_at(x0, y1), value_at(x1, y1)};
            std::array<bool, 4> valid{};
            bool has_invalid = false;
            for (std::size_t tap = 0; tap < valid.size(); ++tap) {
                valid[tap] = std::isfinite(sample_values[tap]) &&
                    sample_values[tap] != configuration_.source_no_data;
                has_invalid = has_invalid || (weights[tap] > 0.0 && !valid[tap]);
            }
            double raw = 0.0;
            bool filled_no_data = false;
            if (!has_invalid) {
                for (std::size_t tap = 0; tap < sample_values.size(); ++tap) {
                    if (weights[tap] > 0.0) {
                        raw += sample_values[tap] * weights[tap];
                    }
                }
            } else if (configuration_.no_data_policy == NoDataPolicy::error) {
                continue;
            } else {
                double best_distance = std::numeric_limits<double>::infinity();
                bool found = false;
                constexpr std::array offsets_x{0.0, 1.0, 0.0, 1.0};
                constexpr std::array offsets_y{0.0, 0.0, 1.0, 1.0};
                for (std::size_t tap = 0; tap < sample_values.size(); ++tap) {
                    if (!valid[tap]) {
                        continue;
                    }
                    const double dx = fx - offsets_x[tap];
                    const double dy = fy - offsets_y[tap];
                    const double distance = dx * dx + dy * dy;
                    if (distance < best_distance) {
                        raw = sample_values[tap];
                        best_distance = distance;
                        found = true;
                    }
                }
                if (!found) {
                    continue;
                }
                filled_no_data = true;
            }
            double elevation = raw * configuration_.sample_scale + configuration_.sample_offset;
            if (configuration_.elevation_representation == ElevationRepresentation::radius_meters) {
                elevation -= configuration_.source_reference_radius_meters;
            }
            if (!std::isfinite(elevation)) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::invalid_argument,
                    "normalized batch raster elevation is non-finite",
                    raster_path_);
            }
            samples[index] = RawTerrainSample{elevation, x0 != x1 || y0 != y1, filled_no_data};
        }
        auto quality = ApplyQualityBatch(
            samples,
            pixels,
            window_x0,
            window_y0,
            window_x1,
            window_y1);
        if (!quality) {
            return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                std::move(quality).error());
        }
        return Result<std::vector<std::optional<RawTerrainSample>>>::success(std::move(samples));
    }

    [[nodiscard]] Result<void> EnsureFullRasterCache() const {
        auto checked = check_telemetry(telemetry_);
        if (!checked) {
            return checked;
        }
        if (full_raster_cache_attempted_) {
            return Result<void>::success();
        }
        TelemetryActivity cache_activity{telemetry_, "decoded_cache"};
        full_raster_cache_attempted_ = true;
        const std::size_t sample_count =
            static_cast<std::size_t>(dataset_handle_->GetRasterXSize()) *
            static_cast<std::size_t>(dataset_handle_->GetRasterYSize());
        const GDALDataType data_type = band_->GetRasterDataType();
        if (!decoded_cache_path_.empty() && raster_artifact_hash_) {
            std::error_code filesystem_error;
            const bool exists = std::filesystem::exists(
                decoded_cache_path_, filesystem_error);
            if (filesystem_error) {
                return Result<void>::failure(raster_error(
                    ErrorCode::io_error,
                    fmt::format(
                        "could not inspect decoded-raster cache: {}",
                        filesystem_error.message()),
                    decoded_cache_path_));
            }
            if (exists && data_type == GDT_Int16) {
                if (telemetry_ != nullptr) {
                    telemetry_->RecordDecodedCacheHit();
                }
                auto loaded = load_decoded_cache(
                    decoded_cache_path_,
                    *raster_artifact_hash_,
                    details_.width,
                    details_.height,
                    full_raster_i16_,
                    telemetry_);
                if (loaded) {
                    AccountDecodedCacheBytes(full_raster_i16_.size() * sizeof(std::int16_t));
                }
                return loaded;
            }
            if (exists && data_type == GDT_Float32) {
                if (telemetry_ != nullptr) {
                    telemetry_->RecordDecodedCacheHit();
                }
                auto loaded = load_decoded_cache(
                    decoded_cache_path_,
                    *raster_artifact_hash_,
                    details_.width,
                    details_.height,
                    full_raster_f32_,
                    telemetry_);
                if (loaded) {
                    AccountDecodedCacheBytes(full_raster_f32_.size() * sizeof(float));
                }
                return loaded;
            }
            if (telemetry_ != nullptr) {
                telemetry_->RecordDecodedCacheMiss();
            }
        }
        auto io_options = raster_io_options(telemetry_);
        if (data_type == GDT_Int16) {
            full_raster_i16_.resize(sample_count);
            if (band_->RasterIO(
                    GF_Read,
                    0,
                    0,
                    dataset_handle_->GetRasterXSize(),
                    dataset_handle_->GetRasterYSize(),
                    full_raster_i16_.data(),
                    dataset_handle_->GetRasterXSize(),
                    dataset_handle_->GetRasterYSize(),
                    GDT_Int16,
                    0,
                    0,
                    &io_options) != CE_None) {
                full_raster_i16_.clear();
                checked = check_telemetry(telemetry_);
                if (!checked) {
                    return checked;
                }
                return Result<void>::failure(raster_error(
                    ErrorCode::io_error,
                    fmt::format("GDAL full-raster cache read failed: {}", CPLGetLastErrorMsg()),
                    raster_path_));
            }
        } else if (data_type == GDT_Float32) {
            full_raster_f32_.resize(sample_count);
            if (band_->RasterIO(
                    GF_Read,
                    0,
                    0,
                    dataset_handle_->GetRasterXSize(),
                    dataset_handle_->GetRasterYSize(),
                    full_raster_f32_.data(),
                    dataset_handle_->GetRasterXSize(),
                    dataset_handle_->GetRasterYSize(),
                    GDT_Float32,
                    0,
                    0,
                    &io_options) != CE_None) {
                full_raster_f32_.clear();
                checked = check_telemetry(telemetry_);
                if (!checked) {
                    return checked;
                }
                return Result<void>::failure(raster_error(
                    ErrorCode::io_error,
                    fmt::format("GDAL full-raster cache read failed: {}", CPLGetLastErrorMsg()),
                    raster_path_));
            }
        }
        AccountDecodedCacheBytes(
            full_raster_i16_.size() * sizeof(std::int16_t) +
            full_raster_f32_.size() * sizeof(float));
        if (!decoded_cache_path_.empty() && raster_artifact_hash_) {
            if (data_type == GDT_Int16) {
                return persist_decoded_cache(
                    decoded_cache_path_,
                    *raster_artifact_hash_,
                    details_.width,
                    details_.height,
                    std::span<const std::int16_t>{full_raster_i16_},
                    telemetry_);
            }
            if (data_type == GDT_Float32) {
                return persist_decoded_cache(
                    decoded_cache_path_,
                    *raster_artifact_hash_,
                    details_.width,
                    details_.height,
                    std::span<const float>{full_raster_f32_},
                    telemetry_);
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<std::array<double, 4>> ReadElevationTaps(
        const int x0,
        const int y0,
        const int x1,
        const int y1) const {
        const int width = x1 - x0 + 1;
        const int height = y1 - y0 + 1;
        const auto cached_value = [&](const int x, const int y) {
            const std::size_t index =
                static_cast<std::size_t>(y) * dataset_handle_->GetRasterXSize() +
                static_cast<std::size_t>(x);
            return full_raster_i16_.empty()
                ? static_cast<double>(full_raster_f32_[index])
                : static_cast<double>(full_raster_i16_[index]);
        };
        if (!full_raster_i16_.empty() || !full_raster_f32_.empty()) {
            return Result<std::array<double, 4>>::success(std::array{
                cached_value(x0, y0),
                cached_value(x1, y0),
                cached_value(x0, y1),
                cached_value(x1, y1)});
        }
        std::array<double, 4> values{};
        if (band_->RasterIO(
                GF_Read,
                x0,
                y0,
                width,
                height,
                values.data(),
                width,
                height,
                GDT_Float64,
                0,
                0,
                nullptr) != CE_None) {
            return failure<std::array<double, 4>>(
                ErrorCode::io_error,
                fmt::format("GDAL sampling failed: {}", CPLGetLastErrorMsg()),
                raster_path_);
        }
        return Result<std::array<double, 4>>::success(std::array{
            values[0],
            values[static_cast<std::size_t>(x1 - x0)],
            values[static_cast<std::size_t>(y1 - y0) * width],
            values[static_cast<std::size_t>(y1 - y0) * width + (x1 - x0)]});
    }

    [[nodiscard]] Result<std::vector<std::array<double, 4>>> ReadQualityTaps(
        const std::array<double, 2> pixel) const {
        const double centered_x = pixel[0] - 0.5;
        const double centered_y = pixel[1] - 0.5;
        const int maximum_x = dataset_handle_->GetRasterXSize() - 1;
        const int maximum_y = dataset_handle_->GetRasterYSize() - 1;
        const int x0 = std::clamp(static_cast<int>(std::floor(centered_x)), 0, maximum_x);
        const int y0 = std::clamp(static_cast<int>(std::floor(centered_y)), 0, maximum_y);
        const int x1 = std::min(x0 + 1, maximum_x);
        const int y1 = std::min(y0 + 1, maximum_y);
        const int width = x1 - x0 + 1;
        const int height = y1 - y0 + 1;
        std::vector<std::array<double, 4>> result;
        result.reserve(quality_rasters_.size());
        for (const QualityRaster& quality : quality_rasters_) {
            if (!quality.cache_u8.empty() || !quality.cache_f32.empty()) {
                const auto cached_value = [&](const int x, const int y) {
                    const std::size_t index =
                        static_cast<std::size_t>(y) * dataset_handle_->GetRasterXSize() +
                        static_cast<std::size_t>(x);
                    return quality.cache_u8.empty()
                        ? static_cast<double>(quality.cache_f32[index])
                        : static_cast<double>(quality.cache_u8[index]);
                };
                result.push_back(std::array{
                    cached_value(x0, y0),
                    cached_value(x1, y0),
                    cached_value(x0, y1),
                    cached_value(x1, y1)});
                continue;
            }
            std::array<double, 4> values{};
            if (quality.band->RasterIO(
                    GF_Read,
                    x0,
                    y0,
                    width,
                    height,
                    values.data(),
                    width,
                    height,
                    GDT_Float64,
                    0,
                    0,
                    nullptr) != CE_None) {
                return failure<std::vector<std::array<double, 4>>>(
                    ErrorCode::io_error,
                    fmt::format("GDAL quality sampling failed: {}", CPLGetLastErrorMsg()),
                    quality.path);
            }
            result.push_back(std::array{
                values[0],
                values[static_cast<std::size_t>(x1 - x0)],
                values[static_cast<std::size_t>(y1 - y0) * width],
                values[static_cast<std::size_t>(y1 - y0) * width + (x1 - x0)]});
        }
        return Result<std::vector<std::array<double, 4>>>::success(std::move(result));
    }

    [[nodiscard]] Result<void> EnsureQualityCaches() const {
        auto checked = check_telemetry(telemetry_);
        if (!checked) {
            return checked;
        }
        TelemetryActivity cache_activity{telemetry_, "decoded_cache"};
        const std::size_t sample_count =
            static_cast<std::size_t>(dataset_handle_->GetRasterXSize()) *
            static_cast<std::size_t>(dataset_handle_->GetRasterYSize());
        for (const QualityRaster& quality : quality_rasters_) {
            checked = check_telemetry(telemetry_);
            if (!checked) {
                return checked;
            }
            if (quality.cache_attempted) {
                continue;
            }
            quality.cache_attempted = true;
            if (!quality.decoded_cache_path.empty() && quality.artifact_hash) {
                std::error_code filesystem_error;
                const bool exists = std::filesystem::exists(
                    quality.decoded_cache_path, filesystem_error);
                if (filesystem_error) {
                    return Result<void>::failure(raster_error(
                        ErrorCode::io_error,
                        fmt::format(
                            "could not inspect decoded quality cache: {}",
                            filesystem_error.message()),
                        quality.decoded_cache_path));
                }
                if (exists && quality.band->GetRasterDataType() == GDT_Byte) {
                    if (telemetry_ != nullptr) {
                        telemetry_->RecordDecodedCacheHit();
                    }
                    auto loaded = load_decoded_cache(
                        quality.decoded_cache_path,
                        *quality.artifact_hash,
                        details_.width,
                        details_.height,
                        quality.cache_u8,
                        telemetry_);
                    if (!loaded) {
                        return loaded;
                    }
                    AccountDecodedCacheBytes(quality.cache_u8.size());
                    continue;
                }
                if (exists) {
                    if (telemetry_ != nullptr) {
                        telemetry_->RecordDecodedCacheHit();
                    }
                    auto loaded = load_decoded_cache(
                        quality.decoded_cache_path,
                        *quality.artifact_hash,
                        details_.width,
                        details_.height,
                        quality.cache_f32,
                        telemetry_);
                    if (!loaded) {
                        return loaded;
                    }
                    AccountDecodedCacheBytes(quality.cache_f32.size() * sizeof(float));
                    continue;
                }
                if (telemetry_ != nullptr) {
                    telemetry_->RecordDecodedCacheMiss();
                }
            }
            auto io_options = raster_io_options(telemetry_);
            if (quality.band->GetRasterDataType() == GDT_Byte) {
                quality.cache_u8.resize(sample_count);
                if (quality.band->RasterIO(
                        GF_Read,
                        0,
                        0,
                        dataset_handle_->GetRasterXSize(),
                        dataset_handle_->GetRasterYSize(),
                        quality.cache_u8.data(),
                        dataset_handle_->GetRasterXSize(),
                        dataset_handle_->GetRasterYSize(),
                        GDT_Byte,
                        0,
                        0,
                        &io_options) != CE_None) {
                    quality.cache_u8.clear();
                    checked = check_telemetry(telemetry_);
                    if (!checked) {
                        return checked;
                    }
                    return Result<void>::failure(raster_error(
                        ErrorCode::io_error,
                        fmt::format("GDAL full quality-cache read failed: {}", CPLGetLastErrorMsg()),
                        quality.path));
                }
                if (!quality.decoded_cache_path.empty() && quality.artifact_hash) {
                    auto persisted = persist_decoded_cache(
                        quality.decoded_cache_path,
                        *quality.artifact_hash,
                        details_.width,
                        details_.height,
                        std::span<const std::uint8_t>{quality.cache_u8},
                        telemetry_);
                    if (!persisted) {
                        return persisted;
                    }
                }
                AccountDecodedCacheBytes(quality.cache_u8.size());
            } else {
                quality.cache_f32.resize(sample_count);
                if (quality.band->RasterIO(
                        GF_Read,
                        0,
                        0,
                        dataset_handle_->GetRasterXSize(),
                        dataset_handle_->GetRasterYSize(),
                        quality.cache_f32.data(),
                        dataset_handle_->GetRasterXSize(),
                        dataset_handle_->GetRasterYSize(),
                        GDT_Float32,
                        0,
                        0,
                        &io_options) != CE_None) {
                    quality.cache_f32.clear();
                    checked = check_telemetry(telemetry_);
                    if (!checked) {
                        return checked;
                    }
                    return Result<void>::failure(raster_error(
                        ErrorCode::io_error,
                        fmt::format("GDAL full quality-cache read failed: {}", CPLGetLastErrorMsg()),
                        quality.path));
                }
                if (!quality.decoded_cache_path.empty() && quality.artifact_hash) {
                    auto persisted = persist_decoded_cache(
                        quality.decoded_cache_path,
                        *quality.artifact_hash,
                        details_.width,
                        details_.height,
                        std::span<const float>{quality.cache_f32},
                        telemetry_);
                    if (!persisted) {
                        return persisted;
                    }
                }
                AccountDecodedCacheBytes(quality.cache_f32.size() * sizeof(float));
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<bool> ApplyQualityValues(
        RawTerrainSample& sample,
        const std::span<const std::array<double, 4>> companions) const {
        if (configuration_.quality_policy == RasterQualityPolicy::none) {
            return Result<bool>::success(true);
        }
        if (configuration_.quality_policy == RasterQualityPolicy::maskelyne_confidence_v1) {
            if (companions.size() != 1U) {
                return failure<bool>(
                    ErrorCode::internal_error,
                    "Maskelyne quality mapping did not receive one companion",
                    raster_path_);
            }
            bool lower_confidence = false;
            for (const double value : companions.front()) {
                if (!std::isfinite(value) || value == 0.0) {
                    return Result<bool>::success(false);
                }
                const auto code = static_cast<std::int32_t>(std::llround(value));
                lower_confidence = lower_confidence || code < 10 || code > 15;
            }
            if (lower_confidence) {
                sample.quality_flags |= quality_interpolated | quality_lower_confidence;
            }
            return Result<bool>::success(true);
        }
        if (companions.size() != 3U) {
            return failure<bool>(
                ErrorCode::internal_error,
                "south-polar quality mapping did not receive three companions",
                raster_path_);
        }
        for (const double count : companions[0]) {
            if (!std::isfinite(count) || count <= 0.0) {
                sample.quality_flags |= quality_filled_no_data | quality_lower_confidence;
            }
        }
        for (const double effective_resolution : companions[1]) {
            if (!std::isfinite(effective_resolution) || effective_resolution > 80.0) {
                sample.quality_flags |= quality_lower_confidence;
            }
        }
        for (const double error : companions[2]) {
            if (!std::isfinite(error) || error > 5.0) {
                sample.quality_flags |= quality_lower_confidence;
            }
        }
        return Result<bool>::success(true);
    }

    [[nodiscard]] Result<bool> ApplyQualitySample(
        RawTerrainSample& sample,
        const std::array<double, 2> pixel) const {
        auto companions = ReadQualityTaps(pixel);
        if (!companions) {
            return Result<bool>::failure(std::move(companions).error());
        }
        return ApplyQualityValues(sample, companions.value());
    }

    [[nodiscard]] Result<void> ApplyQualityBatch(
        std::vector<std::optional<RawTerrainSample>>& samples,
        const std::span<const std::optional<std::array<double, 2>>> pixels,
        const int window_x0,
        const int window_y0,
        const int window_x1,
        const int window_y1) const {
        if (quality_rasters_.empty()) {
            return Result<void>::success();
        }
        const int window_width = window_x1 - window_x0 + 1;
        const int window_height = window_y1 - window_y0 + 1;
        constexpr std::uint64_t maximum_bulk_samples = 4U * 1024U * 1024U;
        const bool bulk = !concurrent_cached_sampling_ &&
            std::uint64_t{static_cast<std::uint32_t>(window_width)} *
                static_cast<std::uint32_t>(window_height) <= maximum_bulk_samples;
        if (!bulk) {
            auto cached = EnsureQualityCaches();
            if (!cached) {
                return cached;
            }
        }
        std::vector<std::vector<double>> grids;
        if (bulk) {
            grids.reserve(quality_rasters_.size());
            for (const QualityRaster& quality : quality_rasters_) {
                auto checked = check_telemetry(telemetry_);
                if (!checked) {
                    return checked;
                }
                grids.emplace_back(static_cast<std::size_t>(window_width) * window_height);
                auto io_options = raster_io_options(telemetry_);
                if (quality.band->RasterIO(
                        GF_Read,
                        window_x0,
                        window_y0,
                        window_width,
                        window_height,
                        grids.back().data(),
                        window_width,
                        window_height,
                        GDT_Float64,
                        0,
                        0,
                        &io_options) != CE_None) {
                    checked = check_telemetry(telemetry_);
                    if (!checked) {
                        return checked;
                    }
                    return Result<void>::failure(raster_error(
                        ErrorCode::io_error,
                        fmt::format("GDAL quality batch sampling failed: {}", CPLGetLastErrorMsg()),
                        quality.path));
                }
            }
        }

        std::vector<std::size_t> order;
        order.reserve(samples.size());
        for (std::size_t index = 0; index < samples.size(); ++index) {
            if (samples[index] && pixels[index]) {
                order.push_back(index);
            }
        }
        if (!bulk) {
            int block_width = 0;
            int block_height = 0;
            quality_rasters_.front().band->GetBlockSize(&block_width, &block_height);
            block_width = std::max(block_width, 1);
            block_height = std::max(block_height, 1);
            std::ranges::sort(order, {}, [&](const std::size_t index) {
                const int x = std::clamp(
                    static_cast<int>(std::floor((*pixels[index])[0] - 0.5)),
                    0,
                    dataset_handle_->GetRasterXSize() - 1);
                const int y = std::clamp(
                    static_cast<int>(std::floor((*pixels[index])[1] - 0.5)),
                    0,
                    dataset_handle_->GetRasterYSize() - 1);
                return std::tuple{y / block_height, x / block_width, y, x, index};
            });
        }

        const int maximum_x = dataset_handle_->GetRasterXSize() - 1;
        const int maximum_y = dataset_handle_->GetRasterYSize() - 1;
        for (const std::size_t index : order) {
            Result<bool> accepted = Result<bool>::success(true);
            if (bulk) {
                const double centered_x = (*pixels[index])[0] - 0.5;
                const double centered_y = (*pixels[index])[1] - 0.5;
                const int x0 = std::clamp(static_cast<int>(std::floor(centered_x)), 0, maximum_x);
                const int y0 = std::clamp(static_cast<int>(std::floor(centered_y)), 0, maximum_y);
                const int x1 = std::min(x0 + 1, maximum_x);
                const int y1 = std::min(y0 + 1, maximum_y);
                std::vector<std::array<double, 4>> companions;
                companions.reserve(grids.size());
                for (const std::vector<double>& grid : grids) {
                    const auto at = [&](const int x, const int y) {
                        return grid[static_cast<std::size_t>(y - window_y0) * window_width +
                                    static_cast<std::size_t>(x - window_x0)];
                    };
                    companions.push_back(std::array{
                        at(x0, y0), at(x1, y0), at(x0, y1), at(x1, y1)});
                }
                accepted = ApplyQualityValues(*samples[index], companions);
            } else {
                accepted = ApplyQualitySample(*samples[index], *pixels[index]);
            }
            if (!accepted) {
                return Result<void>::failure(std::move(accepted).error());
            }
            if (!accepted.value()) {
                samples[index].reset();
            }
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<RawTerrainSample> SampleUnlocked(
        const LunarGeodeticCoordinate coordinate) const {
        if (!Covers(coordinate)) {
            return failure<RawTerrainSample>(
                ErrorCode::not_found, "coordinate is outside raster coverage", raster_path_);
        }
        auto pixel = PixelCoordinate(coordinate);
        if (!pixel) {
            return Result<RawTerrainSample>::failure(std::move(pixel).error());
        }
        const double centered_x = pixel.value()[0] - 0.5;
        const double centered_y = pixel.value()[1] - 0.5;
        const int maximum_x = dataset_handle_->GetRasterXSize() - 1;
        const int maximum_y = dataset_handle_->GetRasterYSize() - 1;
        const int x0 = std::clamp(static_cast<int>(std::floor(centered_x)), 0, maximum_x);
        const int y0 = std::clamp(static_cast<int>(std::floor(centered_y)), 0, maximum_y);
        const int x1 = std::min(x0 + 1, maximum_x);
        const int y1 = std::min(y0 + 1, maximum_y);
        auto taps = ReadElevationTaps(x0, y0, x1, y1);
        if (!taps) {
            return Result<RawTerrainSample>::failure(std::move(taps).error());
        }
        const double fx = std::clamp(centered_x - static_cast<double>(x0), 0.0, 1.0);
        const double fy = std::clamp(centered_y - static_cast<double>(y0), 0.0, 1.0);
        const std::array weights{
            (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
            (1.0 - fx) * fy, fx * fy};
        const std::array sample_values = taps.value();
        std::array<bool, 4> valid{};
        for (std::size_t index = 0; index < valid.size(); ++index) {
            valid[index] = std::isfinite(sample_values[index]) &&
                sample_values[index] != configuration_.source_no_data;
        }
        bool has_invalid = false;
        for (std::size_t index = 0; index < valid.size(); ++index) {
            has_invalid = has_invalid || (weights[index] > 0.0 && !valid[index]);
        }
        double raw = 0.0;
        bool filled_no_data = false;
        if (!has_invalid) {
            for (std::size_t index = 0; index < sample_values.size(); ++index) {
                if (weights[index] > 0.0) {
                    raw += sample_values[index] * weights[index];
                }
            }
        } else if (configuration_.no_data_policy == NoDataPolicy::error) {
            return failure<RawTerrainSample>(
                ErrorCode::invalid_argument,
                "raster interpolation encountered the declared no-data value", raster_path_);
        } else {
            double best_distance = std::numeric_limits<double>::infinity();
            bool found = false;
            const std::array offsets_x{0.0, 1.0, 0.0, 1.0};
            const std::array offsets_y{0.0, 0.0, 1.0, 1.0};
            for (std::size_t index = 0; index < sample_values.size(); ++index) {
                if (!valid[index]) {
                    continue;
                }
                const double dx = fx - offsets_x[index];
                const double dy = fy - offsets_y[index];
                const double distance = dx * dx + dy * dy;
                if (distance < best_distance) {
                    raw = sample_values[index];
                    best_distance = distance;
                    found = true;
                }
            }
            if (!found) {
                return failure<RawTerrainSample>(
                    ErrorCode::invalid_argument,
                    "raster no-data policy found no valid interpolation sample", raster_path_);
            }
            filled_no_data = true;
        }
        double elevation = raw * configuration_.sample_scale + configuration_.sample_offset;
        if (configuration_.elevation_representation == ElevationRepresentation::radius_meters) {
            elevation -= configuration_.source_reference_radius_meters;
        }
        if (!std::isfinite(elevation)) {
            return failure<RawTerrainSample>(
                ErrorCode::invalid_argument, "normalized raster elevation is non-finite", raster_path_);
        }
        return Result<RawTerrainSample>::success(RawTerrainSample{
            elevation,
            x0 != x1 || y0 != y1,
            filled_no_data,
        });
    }

public:
    [[nodiscard]] Result<Sha256Digest> WindowDependency(
        const LunarTileKey key) const override {
        const std::scoped_lock lock{mutex_};
        std::array<LunarGeodeticCoordinate, 4> corners{};
        const std::array<std::uint16_t, 4> x_samples{0, 256, 0, 256};
        const std::array<std::uint16_t, 4> y_samples{0, 0, 256, 256};
        double minimum_x = std::numeric_limits<double>::infinity();
        double maximum_x = -std::numeric_limits<double>::infinity();
        double minimum_y = std::numeric_limits<double>::infinity();
        double maximum_y = -std::numeric_limits<double>::infinity();
        for (std::size_t index = 0; index < corners.size(); ++index) {
            auto u = QscProjection::LatticeCoordinate(key.x(), x_samples[index], key.level());
            auto v = QscProjection::LatticeCoordinate(key.y(), y_samples[index], key.level());
            if (!u || !v) {
                return Result<Sha256Digest>::failure(u ? std::move(v).error() : std::move(u).error());
            }
            auto coordinate = QscProjection::Inverse(QscCoordinate{
                static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
            if (!coordinate) {
                return Result<Sha256Digest>::failure(std::move(coordinate).error());
            }
            auto pixel = PixelCoordinate(coordinate.value());
            if (!pixel) {
                return Result<Sha256Digest>::failure(std::move(pixel).error());
            }
            minimum_x = std::min(minimum_x, pixel.value()[0]);
            maximum_x = std::max(maximum_x, pixel.value()[0]);
            minimum_y = std::min(minimum_y, pixel.value()[1]);
            maximum_y = std::max(maximum_y, pixel.value()[1]);
        }
        const std::int64_t start_x = static_cast<std::int64_t>(std::floor(minimum_x)) - 1;
        const std::int64_t start_y = static_cast<std::int64_t>(std::floor(minimum_y)) - 1;
        const std::uint64_t width = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::ceil(maximum_x)) - start_x + 2);
        const std::uint64_t height = static_cast<std::uint64_t>(
            static_cast<std::int64_t>(std::ceil(maximum_y)) - start_y + 2);
        Bytes input;
        append_domain(input, "LTDB_GDAL_WINDOW_V1");
        append_u64(input, key.encoded());
        append_u64(input, static_cast<std::uint64_t>(start_x));
        append_u64(input, static_cast<std::uint64_t>(start_y));
        append_u64(input, width);
        append_u64(input, height);
        append_u32(input, 1);
        append_f64(input, configuration_.source_no_data);
        append_f64(input, configuration_.sample_scale);
        append_f64(input, configuration_.sample_offset);
        append_f64(input, configuration_.source_reference_radius_meters);
        append_u8(input, static_cast<std::uint8_t>(configuration_.elevation_representation));
        append_u8(input, static_cast<std::uint8_t>(configuration_.no_data_policy));
        input.insert(
            input.end(), dataset_.artifact_bundle_hash.bytes.begin(),
            dataset_.artifact_bundle_hash.bytes.end());
        return sha256(input);
    }

private:
    void AccountDecodedCacheBytes(const std::uint64_t bytes) const {
        if (telemetry_ != nullptr && bytes != 0) {
            telemetry_->AddDecodedCacheLiveBytes(bytes);
            telemetry_->AddCount("decoded_cache_bytes_loaded", bytes);
        }
    }

    GdalRasterSource(
        RasterConfiguration configuration,
        DatasetArtifact dataset,
        DatasetPtr handle,
        std::filesystem::path raster_path,
        std::string driver_name,
        std::vector<QualityRaster> quality_rasters,
        std::optional<Sha256Digest> raster_artifact_hash,
        std::filesystem::path decoded_cache_path,
        TelemetryCollector* const collector)
        : configuration_(std::move(configuration)),
          dataset_(std::move(dataset)),
          dataset_handle_(std::move(handle)),
          raster_path_(std::move(raster_path)),
          quality_rasters_(std::move(quality_rasters)),
          raster_artifact_hash_(raster_artifact_hash),
          decoded_cache_path_(std::move(decoded_cache_path)),
          telemetry_(collector) {
        band_ = dataset_handle_->GetRasterBand(1);
        details_.driver_name = std::move(driver_name);
        details_.data_type_name = band_ == nullptr
            ? std::string{} : std::string{GDALGetDataTypeName(band_->GetRasterDataType())};
        details_.width = static_cast<std::uint32_t>(dataset_handle_->GetRasterXSize());
        details_.height = static_cast<std::uint32_t>(dataset_handle_->GetRasterYSize());
        details_.no_data_value = configuration_.source_no_data;
        details_.sample_scale = configuration_.sample_scale;
        details_.sample_offset = configuration_.sample_offset;
        details_.elevation_representation = configuration_.elevation_representation;
        details_.footprint = GeographicFootprint{
            configuration_.west_longitude_degrees,
            configuration_.east_longitude_degrees,
            configuration_.south_latitude_degrees,
            configuration_.north_latitude_degrees,
        };
    }

    [[nodiscard]] Result<void> InitializeGeoreferencing() {
        if (configuration_.metadata_override) {
            geotransform_ = {
                configuration_.west_longitude_degrees,
                (configuration_.east_longitude_degrees - configuration_.west_longitude_degrees) /
                    static_cast<double>(details_.width),
                0.0,
                configuration_.north_latitude_degrees,
                0.0,
                -(configuration_.north_latitude_degrees - configuration_.south_latitude_degrees) /
                    static_cast<double>(details_.height),
            };
            if (GDALInvGeoTransform(geotransform_.data(), inverse_geotransform_.data()) == 0) {
                return Result<void>::failure(raster_error(
                    ErrorCode::invalid_argument, "configured raster geotransform is not invertible", raster_path_));
            }
            return Result<void>::success();
        }

        if (dataset_handle_->GetGeoTransform(geotransform_.data()) != CE_None ||
            GDALInvGeoTransform(geotransform_.data(), inverse_geotransform_.data()) == 0) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "raster has no invertible geotransform and metadata_override is false", raster_path_));
        }
        const OGRSpatialReference* dataset_spatial_reference = dataset_handle_->GetSpatialRef();
        if (dataset_spatial_reference == nullptr || dataset_spatial_reference->IsEmpty()) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "raster has no projection and metadata_override is false", raster_path_));
        }
        OGRSpatialReference canonical;
        canonical.SetGeogCS(
            "Lunar 2000", "D_Moon_2000", "Moon_2000_IAU_IAG",
            1'737'400.0, 0.0, "Reference_Meridian", 0.0,
            "degree", std::numbers::pi_v<double> / 180.0);
        canonical.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRSpatialReference dataset_copy{*dataset_spatial_reference};
        dataset_copy.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        to_dataset_.reset(OGRCreateCoordinateTransformation(&canonical, &dataset_copy));
        from_dataset_.reset(OGRCreateCoordinateTransformation(&dataset_copy, &canonical));
        if (!to_dataset_ || !from_dataset_) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "could not transform between canonical lunar coordinates and the raster CRS", raster_path_));
        }

        GeographicFootprint extracted{
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::infinity(),
            -std::numeric_limits<double>::infinity(),
        };
        const std::array<std::array<double, 2>, 4> pixels{{
            {0.0, 0.0},
            {static_cast<double>(details_.width), 0.0},
            {0.0, static_cast<double>(details_.height)},
            {static_cast<double>(details_.width), static_cast<double>(details_.height)},
        }};
        for (const auto& pixel : pixels) {
            double x = geotransform_[0] + pixel[0] * geotransform_[1] + pixel[1] * geotransform_[2];
            double y = geotransform_[3] + pixel[0] * geotransform_[4] + pixel[1] * geotransform_[5];
            if (!from_dataset_->Transform(1, &x, &y)) {
                return Result<void>::failure(raster_error(
                    ErrorCode::invalid_argument, "could not extract raster footprint", raster_path_));
            }
            extracted.west_longitude_degrees = std::min(extracted.west_longitude_degrees, x);
            extracted.east_longitude_degrees = std::max(extracted.east_longitude_degrees, x);
            extracted.south_latitude_degrees = std::min(extracted.south_latitude_degrees, y);
            extracted.north_latitude_degrees = std::max(extracted.north_latitude_degrees, y);
        }
        double center_x = geotransform_[0] +
            static_cast<double>(details_.width) * 0.5 * geotransform_[1] +
            static_cast<double>(details_.height) * 0.5 * geotransform_[2];
        double center_y = geotransform_[3] +
            static_cast<double>(details_.width) * 0.5 * geotransform_[4] +
            static_cast<double>(details_.height) * 0.5 * geotransform_[5];
        if (from_dataset_->Transform(1, &center_x, &center_y) &&
            almost_equal_degrees(center_y, -90.0)) {
            extracted.west_longitude_degrees = -180.0;
            extracted.east_longitude_degrees = 180.0;
            extracted.south_latitude_degrees = -90.0;
        }
        if (!almost_equal_degrees(extracted.west_longitude_degrees, details_.footprint.west_longitude_degrees) ||
            !almost_equal_degrees(extracted.east_longitude_degrees, details_.footprint.east_longitude_degrees) ||
            !almost_equal_degrees(extracted.south_latitude_degrees, details_.footprint.south_latitude_degrees) ||
            !almost_equal_degrees(extracted.north_latitude_degrees, details_.footprint.north_latitude_degrees)) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                fmt::format(
                    "GDAL raster footprint [{}, {}, {}, {}] disagrees with declared bounds [{}, {}, {}, {}]",
                    extracted.west_longitude_degrees,
                    extracted.east_longitude_degrees,
                    extracted.south_latitude_degrees,
                    extracted.north_latitude_degrees,
                    details_.footprint.west_longitude_degrees,
                    details_.footprint.east_longitude_degrees,
                    details_.footprint.south_latitude_degrees,
                    details_.footprint.north_latitude_degrees),
                raster_path_));
        }
        details_.footprint = extracted;
        return Result<void>::success();
    }

    [[nodiscard]] Result<void> ValidateBandMetadata() const {
        if (band_ == nullptr) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument, "GDAL raster does not contain band 1", raster_path_));
        }
        if (details_.data_type_name != configuration_.expected_data_type) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                fmt::format("GDAL band type is '{}', expected '{}'",
                            details_.data_type_name, configuration_.expected_data_type),
                raster_path_));
        }
        if (configuration_.metadata_override) {
            return Result<void>::success();
        }
        int no_data_success = 0;
        const double no_data = band_->GetNoDataValue(&no_data_success);
        int scale_success = 0;
        const double scale = band_->GetScale(&scale_success);
        int offset_success = 0;
        const double offset = band_->GetOffset(&offset_success);
        if (no_data_success == 0 ||
            !(almost_equal(no_data, configuration_.source_no_data) ||
              (std::isnan(no_data) && std::isnan(configuration_.source_no_data)))) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "GDAL no-data metadata is missing or disagrees with configuration", raster_path_));
        }
        if ((scale_success == 0 && configuration_.sample_scale != 1.0) ||
            (scale_success != 0 && !almost_equal(scale, configuration_.sample_scale))) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "GDAL sample scale is missing or disagrees with configuration", raster_path_));
        }
        if ((offset_success == 0 && configuration_.sample_offset != 0.0) ||
            (offset_success != 0 && !almost_equal(offset, configuration_.sample_offset))) {
            return Result<void>::failure(raster_error(
                ErrorCode::invalid_argument,
                "GDAL sample offset is missing or disagrees with configuration", raster_path_));
        }
        return Result<void>::success();
    }

    [[nodiscard]] Result<std::array<double, 2>> PixelCoordinate(
        const LunarGeodeticCoordinate coordinate) const {
        double x = coordinate.longitude_radians * radians_to_degrees;
        if (details_.footprint.west_longitude_degrees >= 0.0 && x < 0.0) {
            x += 360.0;
        }
        double y = coordinate.latitude_radians * radians_to_degrees;
        if (to_dataset_ && !to_dataset_->Transform(1, &x, &y)) {
            return failure<std::array<double, 2>>(
                ErrorCode::invalid_argument,
                "could not project a canonical coordinate into the raster CRS", raster_path_);
        }
        const double pixel = inverse_geotransform_[0] + inverse_geotransform_[1] * x +
            inverse_geotransform_[2] * y;
        const double line = inverse_geotransform_[3] + inverse_geotransform_[4] * x +
            inverse_geotransform_[5] * y;
        if (!std::isfinite(pixel) || !std::isfinite(line)) {
            return failure<std::array<double, 2>>(
                ErrorCode::invalid_argument, "raster coordinate transform produced non-finite pixels", raster_path_);
        }
        return Result<std::array<double, 2>>::success({pixel, line});
    }

    RasterConfiguration configuration_;
    DatasetArtifact dataset_;
    DatasetPtr dataset_handle_;
    GDALRasterBand* band_{};
    std::filesystem::path raster_path_;
    RasterSourceDetails details_;
    std::array<double, 6> geotransform_{};
    std::array<double, 6> inverse_geotransform_{};
    CoordinateTransformationPtr to_dataset_;
    CoordinateTransformationPtr from_dataset_;
    std::vector<QualityRaster> quality_rasters_;
    std::optional<Sha256Digest> raster_artifact_hash_;
    std::filesystem::path decoded_cache_path_;
    mutable bool full_raster_cache_attempted_{};
    mutable std::vector<std::int16_t> full_raster_i16_;
    mutable std::vector<float> full_raster_f32_;
    bool concurrent_cached_sampling_{};
    TelemetryCollector* telemetry_{};
    mutable std::mutex mutex_;
};

class MosaicRasterSource final : public IRasterSource {
public:
    static Result<std::unique_ptr<IRasterSource>> Open(
        const RasterConfiguration& configuration,
        DatasetArtifact dataset,
        const std::optional<std::filesystem::path>& decoded_cache_root,
        TelemetryCollector* const collector) {
        if (configuration.raster_files.empty()) {
            return failure<std::unique_ptr<IRasterSource>>(
                ErrorCode::invalid_argument, "logical raster source contains no raster files");
        }
        std::vector<std::unique_ptr<IRasterSource>> components;
        components.reserve(configuration.raster_files.size());
        for (const RasterFileConfiguration& file : configuration.raster_files) {
            RasterConfiguration physical = configuration;
            physical.raster_member = file.member;
            physical.expected_width = file.expected_width;
            physical.expected_height = file.expected_height;
            physical.west_longitude_degrees = file.bounds.west_longitude_degrees;
            physical.east_longitude_degrees = file.bounds.east_longitude_degrees;
            physical.south_latitude_degrees = file.bounds.south_latitude_degrees;
            physical.north_latitude_degrees = file.bounds.north_latitude_degrees;
            auto opened = GdalRasterSource::Open(
                physical, dataset, decoded_cache_root, collector);
            if (!opened) {
                return Result<std::unique_ptr<IRasterSource>>::failure(std::move(opened).error());
            }
            components.push_back(std::move(opened).value());
        }
        return Result<std::unique_ptr<IRasterSource>>::success(
            std::unique_ptr<IRasterSource>{new MosaicRasterSource(
                std::move(dataset), std::move(components))});
    }

    [[nodiscard]] const DatasetArtifact& metadata() const noexcept override { return dataset_; }
    [[nodiscard]] const RasterSourceDetails& details() const noexcept override { return details_; }

    [[nodiscard]] bool Covers(const LunarGeodeticCoordinate coordinate) const noexcept override {
        return std::ranges::any_of(components_, [&](const auto& source) {
            return source->Covers(coordinate);
        });
    }

    [[nodiscard]] Result<RawTerrainSample> Sample(
        const LunarGeodeticCoordinate coordinate) const override {
        for (const auto& source : components_) {
            auto sample = source->TrySample(coordinate);
            if (!sample) {
                return Result<RawTerrainSample>::failure(std::move(sample).error());
            }
            if (sample.value()) {
                return Result<RawTerrainSample>::success(*sample.value());
            }
        }
        return failure<RawTerrainSample>(
            ErrorCode::not_found, "coordinate is outside the logical raster source coverage or is no-data");
    }

    [[nodiscard]] Result<std::vector<std::optional<RawTerrainSample>>> SampleBatch(
        const std::span<const LunarGeodeticCoordinate> coordinates,
        const std::stop_token cancellation) const override {
        std::vector<std::optional<RawTerrainSample>> samples(coordinates.size());
        std::vector<std::optional<std::vector<std::optional<RawTerrainSample>>>> component_samples(
            components_.size());
        std::vector<std::size_t> active_components;
        active_components.reserve(components_.size());
        for (std::size_t component_index = 0; component_index < components_.size();
             ++component_index) {
            if (cancellation.stop_requested()) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::cancelled, "mosaic sampling was cancelled");
            }
            if (std::ranges::any_of(coordinates, [&](const LunarGeodeticCoordinate coordinate) {
                    return components_[component_index]->Covers(coordinate);
                })) {
                active_components.push_back(component_index);
            }
        }
        std::vector<BuilderTask> tasks;
        tasks.reserve(active_components.size());
        for (const std::size_t component_index : active_components) {
            tasks.emplace_back([&, component_index]() -> Result<void> {
                auto sampled = components_[component_index]->SampleBatch(
                    coordinates, cancellation);
                if (!sampled) {
                    return Result<void>::failure(std::move(sampled).error());
                }
                component_samples[component_index] = std::move(sampled).value();
                return Result<void>::success();
            });
        }
        Result<void> executed = Result<void>::success();
        if (tasks.size() == 1U) {
            executed = tasks.front()();
        } else if (!tasks.empty()) {
            const std::uint32_t parallelism = std::min<std::uint32_t>(
                static_cast<std::uint32_t>(active_components.size()),
                std::max(1U, std::min(4U, std::thread::hardware_concurrency())));
            executed = run_bounded_tasks(tasks, parallelism, cancellation);
        }
        if (!executed) {
            return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                std::move(executed).error());
        }
        for (const std::size_t component_index : active_components) {
            if (cancellation.stop_requested()) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::cancelled, "mosaic sampling was cancelled");
            }
            if (!component_samples[component_index]) {
                return failure<std::vector<std::optional<RawTerrainSample>>>(
                    ErrorCode::internal_error,
                    "logical raster batch omitted a physical component result");
            }
            for (std::size_t index = 0; index < samples.size(); ++index) {
                if (!samples[index] && (*component_samples[component_index])[index]) {
                    samples[index] = std::move((*component_samples[component_index])[index]);
                }
            }
        }
        return Result<std::vector<std::optional<RawTerrainSample>>>::success(
            std::move(samples));
    }

    [[nodiscard]] Result<Sha256Digest> WindowDependency(
        const LunarTileKey key) const override {
        Bytes input;
        append_domain(input, "LTDB_GDAL_MOSAIC_WINDOW_V1");
        append_u64(input, key.encoded());
        append_u64(input, components_.size());
        for (const auto& source : components_) {
            auto dependency = source->WindowDependency(key);
            if (!dependency) {
                return Result<Sha256Digest>::failure(std::move(dependency).error());
            }
            input.insert(input.end(), dependency.value().bytes.begin(), dependency.value().bytes.end());
        }
        return sha256(input);
    }

private:
    MosaicRasterSource(
        DatasetArtifact dataset,
        std::vector<std::unique_ptr<IRasterSource>> components)
        : dataset_(std::move(dataset)), components_(std::move(components)) {
        details_ = components_.front()->details();
        details_.width = 0;
        details_.height = 0;
        for (const auto& source : components_) {
            const GeographicFootprint& footprint = source->details().footprint;
            details_.footprint.west_longitude_degrees = std::min(
                details_.footprint.west_longitude_degrees, footprint.west_longitude_degrees);
            details_.footprint.east_longitude_degrees = std::max(
                details_.footprint.east_longitude_degrees, footprint.east_longitude_degrees);
            details_.footprint.south_latitude_degrees = std::min(
                details_.footprint.south_latitude_degrees, footprint.south_latitude_degrees);
            details_.footprint.north_latitude_degrees = std::max(
                details_.footprint.north_latitude_degrees, footprint.north_latitude_degrees);
        }
        if (details_.footprint.east_longitude_degrees -
                details_.footprint.west_longitude_degrees >= 360.0 - 1.0e-7) {
            details_.footprint.west_longitude_degrees = -180.0;
            details_.footprint.east_longitude_degrees = 180.0;
        }
    }

    DatasetArtifact dataset_;
    std::vector<std::unique_ptr<IRasterSource>> components_;
    RasterSourceDetails details_;
};

[[nodiscard]] Result<std::optional<std::filesystem::path>> decoded_cache_environment() {
    std::string value;
#ifdef _WIN32
    char* buffer = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&buffer, &length, "LUNAR_TERRAIN_DECODED_CACHE") != 0) {
        return failure<std::optional<std::filesystem::path>>(
            ErrorCode::io_error,
            "could not read LUNAR_TERRAIN_DECODED_CACHE");
    }
    if (buffer != nullptr) {
        value.assign(buffer);
        std::free(buffer);
    }
#else
    if (const char* buffer = std::getenv("LUNAR_TERRAIN_DECODED_CACHE");
        buffer != nullptr) {
        value.assign(buffer);
    }
#endif
    if (value.empty()) {
        return Result<std::optional<std::filesystem::path>>::success(std::nullopt);
    }
    std::filesystem::path path{value};
    if (!path.is_absolute()) {
        return failure<std::optional<std::filesystem::path>>(
            ErrorCode::invalid_argument,
            "LUNAR_TERRAIN_DECODED_CACHE must be an absolute local path",
            path);
    }
    return Result<std::optional<std::filesystem::path>>::success(
        std::optional{std::move(path)});
}

[[nodiscard]] Result<std::unique_ptr<IRasterSource>> open_logical_raster(
    const RasterConfiguration& configuration,
    DatasetArtifact dataset,
    TelemetryCollector* const collector) {
    auto decoded_cache_root = decoded_cache_environment();
    if (!decoded_cache_root) {
        return Result<std::unique_ptr<IRasterSource>>::failure(
            std::move(decoded_cache_root).error());
    }
    if (configuration.raster_files.size() <= 1U) {
        return GdalRasterSource::Open(
            configuration, std::move(dataset), decoded_cache_root.value(), collector);
    }
    return MosaicRasterSource::Open(
        configuration, std::move(dataset), decoded_cache_root.value(), collector);
}

}  // namespace

CoverageIndex::CoverageIndex(const std::span<const IRasterSource* const> sources)
    : sources_(sources.begin(), sources.end()) {
    std::ranges::sort(sources_, {}, [](const IRasterSource* source) {
        return source->metadata().id.value;
    });
}

std::vector<const IRasterSource*> CoverageIndex::At(
    const LunarGeodeticCoordinate coordinate) const {
    std::vector<const IRasterSource*> result;
    for (const IRasterSource* source : sources_) {
        if (source->Covers(coordinate)) {
            result.push_back(source);
        }
    }
    return result;
}

Result<std::vector<const IRasterSource*>> CoverageIndex::Covering(
    const LunarTileKey key) const {
    std::vector<const IRasterSource*> result;
    constexpr std::array<std::uint16_t, 3> samples{0, 128, 256};
    for (const IRasterSource* source : sources_) {
        bool covered = true;
        for (const std::uint16_t y : samples) {
            for (const std::uint16_t x : samples) {
                auto u = QscProjection::LatticeCoordinate(key.x(), x, key.level());
                auto v = QscProjection::LatticeCoordinate(key.y(), y, key.level());
                if (!u || !v) {
                    return Result<std::vector<const IRasterSource*>>::failure(
                        u ? std::move(v).error() : std::move(u).error());
                }
                auto coordinate = QscProjection::Inverse(QscCoordinate{
                    static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
                if (!coordinate) {
                    return Result<std::vector<const IRasterSource*>>::failure(
                        std::move(coordinate).error());
                }
                covered = covered && source->Covers(coordinate.value());
            }
        }
        if (covered) {
            result.push_back(source);
        }
    }
    return Result<std::vector<const IRasterSource*>>::success(std::move(result));
}

Result<std::unique_ptr<IRasterSource>> open_raster_source(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    TelemetryCollector* const collector) {
    if (configuration.source_kind != BuilderSourceKind::raster || configuration.rasters.size() != 1) {
        return failure<std::unique_ptr<IRasterSource>>(
            ErrorCode::invalid_argument, "configuration does not select a raster source");
    }
    TelemetryActivity hashing{collector, "source_hashing"};
    auto members = catalog_artifacts(configuration.rasters.front(), collector);
    if (!members) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(members).error());
    }
    auto bundle = artifact_bundle_identity(members.value(), configuration.rasters.front());
    if (!bundle) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(bundle).error());
    }
    const std::size_t member_count = members.value().size();
    auto dataset = make_dataset_artifact(
        configuration.rasters.front(),
        identity.dataset_ids.front(),
        std::move(members).value(),
        bundle.value().first,
        bundle.value().second);
    if (!dataset) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(dataset).error());
    }
    if (collector != nullptr) {
        collector->AddCount("source_members_hashed", member_count);
    }
    TelemetryActivity opening{collector, "source_opening"};
    return open_logical_raster(
        configuration.rasters.front(), std::move(dataset).value(), collector);
}

Result<std::optional<RawTerrainSample>> IRasterSource::TrySample(
    const LunarGeodeticCoordinate coordinate) const {
    if (!Covers(coordinate)) {
        return Result<std::optional<RawTerrainSample>>::success(std::nullopt);
    }
    auto sample = Sample(coordinate);
    if (!sample) {
        if (sample.error().code == ErrorCode::not_found ||
            (sample.error().code == ErrorCode::invalid_argument &&
             sample.error().message.find("no-data") != std::string::npos)) {
            return Result<std::optional<RawTerrainSample>>::success(std::nullopt);
        }
        return Result<std::optional<RawTerrainSample>>::failure(std::move(sample).error());
    }
    return Result<std::optional<RawTerrainSample>>::success(std::move(sample).value());
}

Result<std::vector<std::optional<RawTerrainSample>>> IRasterSource::SampleBatch(
    const std::span<const LunarGeodeticCoordinate> coordinates,
    const std::stop_token cancellation) const {
    std::vector<std::optional<RawTerrainSample>> samples;
    samples.reserve(coordinates.size());
    for (std::size_t index = 0; index < coordinates.size(); ++index) {
        if ((index % 256U) == 0U && cancellation.stop_requested()) {
            return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                Error{ErrorCode::cancelled, "raster sampling was cancelled"});
        }
        const LunarGeodeticCoordinate coordinate = coordinates[index];
        auto sample = TrySample(coordinate);
        if (!sample) {
            return Result<std::vector<std::optional<RawTerrainSample>>>::failure(
                std::move(sample).error());
        }
        samples.push_back(std::move(sample).value());
    }
    return Result<std::vector<std::optional<RawTerrainSample>>>::success(
        std::move(samples));
}

Result<std::vector<std::unique_ptr<IRasterSource>>> open_raster_sources(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity,
    TelemetryCollector* const collector) {
    if (configuration.source_kind != BuilderSourceKind::raster ||
        configuration.rasters.empty() ||
        configuration.rasters.size() != identity.dataset_ids.size()) {
        return failure<std::vector<std::unique_ptr<IRasterSource>>>(
            ErrorCode::invalid_argument, "configuration does not select valid raster sources");
    }
    std::vector<std::unique_ptr<IRasterSource>> sources;
    sources.reserve(configuration.rasters.size());
    for (std::size_t index = 0; index < configuration.rasters.size(); ++index) {
        if (collector != nullptr) {
            auto checked = collector->Check();
            if (!checked) {
                return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                    std::move(checked).error());
            }
            collector->SetWork(index, configuration.rasters.size());
        }
        const RasterConfiguration& raster = configuration.rasters[index];
        Result<std::vector<ArtifactMember>> members = [&]() {
            TelemetryActivity activity{collector, "source_hashing"};
            return catalog_artifacts(raster, collector);
        }();
        if (!members) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(members).error());
        }
        auto bundle = artifact_bundle_identity(members.value(), raster);
        if (!bundle) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(bundle).error());
        }
        const std::size_t member_count = members.value().size();
        auto dataset = make_dataset_artifact(
            raster,
            identity.dataset_ids[index],
            std::move(members).value(),
            bundle.value().first,
            bundle.value().second);
        if (!dataset) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(dataset).error());
        }
        if (collector != nullptr) {
            collector->AddCount("source_members_hashed", member_count);
        }
        auto source = [&]() {
            TelemetryActivity activity{collector, "source_opening"};
            return open_logical_raster(raster, std::move(dataset).value(), collector);
        }();
        if (!source) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(source).error());
        }
        sources.push_back(std::move(source).value());
    }
    if (collector != nullptr) {
        collector->SetWork(configuration.rasters.size(), configuration.rasters.size());
        collector->AddCount("sources_opened", configuration.rasters.size());
    }
    return Result<std::vector<std::unique_ptr<IRasterSource>>>::success(std::move(sources));
}

Result<PreparedSourceCatalog> prepare_source_catalog(
    const BuilderConfiguration& configuration,
    TelemetryCollector* const collector) {
    if (configuration.source_kind != BuilderSourceKind::raster) {
        return Result<PreparedSourceCatalog>::failure(Error{
            ErrorCode::invalid_argument,
            "a prepared source catalog requires a raster configuration"});
    }
    auto identity = identify_configuration(configuration);
    if (!identity) {
        return Result<PreparedSourceCatalog>::failure(std::move(identity).error());
    }
    auto sources = open_raster_sources(configuration, identity.value(), collector);
    if (!sources) {
        return Result<PreparedSourceCatalog>::failure(std::move(sources).error());
    }
    return Result<PreparedSourceCatalog>::success(PreparedSourceCatalog{
        std::move(identity).value(), std::move(sources).value()});
}

Result<LunarTileKey> choose_raster_prototype_tile(
    const IRasterSource& source,
    const std::uint8_t level) {
    const GeographicFootprint& footprint = source.details().footprint;
    double longitude = (footprint.west_longitude_degrees +
                        footprint.east_longitude_degrees) * 0.5;
    if (longitude > 180.0) {
        longitude -= 360.0;
    }
    const double latitude = (footprint.south_latitude_degrees +
                             footprint.north_latitude_degrees) * 0.5;
    auto qsc = QscProjection::Forward(LunarGeodeticCoordinate{
        latitude / radians_to_degrees,
        longitude / radians_to_degrees,
        0.0,
    });
    if (!qsc) {
        return Result<LunarTileKey>::failure(std::move(qsc).error());
    }
    const std::uint32_t tiles_per_axis = std::uint32_t{1} << level;
    const auto tile_coordinate = [tiles_per_axis](const double value) {
        const double scaled = (value + 1.0) * 0.5 * static_cast<double>(tiles_per_axis);
        return std::min(
            static_cast<std::uint32_t>(std::floor(scaled)), tiles_per_axis - 1U);
    };
    auto key = LunarTileKey::create(
        static_cast<std::uint8_t>(qsc.value().face), level,
        tile_coordinate(qsc.value().u), tile_coordinate(qsc.value().v));
    if (!key) {
        return key;
    }
    const IRasterSource* source_pointer = &source;
    CoverageIndex coverage{std::span{&source_pointer, std::size_t{1}}};
    auto covering = coverage.Covering(key.value());
    if (!covering) {
        return Result<LunarTileKey>::failure(std::move(covering).error());
    }
    if (covering.value().empty()) {
        return failure<LunarTileKey>(
            ErrorCode::invalid_argument,
            "the centered prototype QSC tile is not wholly covered by the raster footprint");
    }
    return key;
}

}  // namespace lunar::terrain::builder
