#include "builder/raster_source.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
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

[[nodiscard]] Result<Sha256Digest> hash_file(const std::filesystem::path& path) {
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
        stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize bytes = stream.gcount();
        if (bytes > 0 && EVP_DigestUpdate(
                context.get(), buffer.data(), static_cast<std::size_t>(bytes)) != 1) {
            return failure<Sha256Digest>(ErrorCode::internal_error, "could not update SHA-256", path);
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

[[nodiscard]] Result<std::vector<ArtifactMember>> catalog_artifacts(
    const RasterConfiguration& configuration) {
    std::vector<ArtifactMember> members;
    members.reserve(configuration.artifact_members.size());
    for (const ArtifactMemberConfiguration& expected : configuration.artifact_members) {
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
        auto digest = hash_file(path);
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

class GdalRasterSource final : public IRasterSource {
public:
    static Result<std::unique_ptr<IRasterSource>> Open(
        const RasterConfiguration& configuration,
        DatasetArtifact dataset) {
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

        auto source = std::unique_ptr<GdalRasterSource>{new GdalRasterSource(
            configuration, std::move(dataset), std::move(opened), raster_path, driver_name)};
        auto initialized = source->InitializeGeoreferencing();
        if (!initialized) {
            return Result<std::unique_ptr<IRasterSource>>::failure(std::move(initialized).error());
        }
        auto metadata = source->ValidateBandMetadata();
        if (!metadata) {
            return Result<std::unique_ptr<IRasterSource>>::failure(std::move(metadata).error());
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
        const int width = x1 - x0 + 1;
        const int height = y1 - y0 + 1;
        std::array<double, 4> values{};
        if (band_->RasterIO(
                GF_Read, x0, y0, width, height, values.data(), width, height,
                GDT_Float64, 0, 0, nullptr) != CE_None) {
            return failure<RawTerrainSample>(
                ErrorCode::io_error,
                fmt::format("GDAL sampling failed: {}", CPLGetLastErrorMsg()), raster_path_);
        }
        const double fx = std::clamp(centered_x - static_cast<double>(x0), 0.0, 1.0);
        const double fy = std::clamp(centered_y - static_cast<double>(y0), 0.0, 1.0);
        const std::array weights{
            (1.0 - fx) * (1.0 - fy), fx * (1.0 - fy),
            (1.0 - fx) * fy, fx * fy};
        const std::array sample_values{
            values[0], values[static_cast<std::size_t>(x1 - x0)],
            values[static_cast<std::size_t>(y1 - y0) * width],
            values[static_cast<std::size_t>(y1 - y0) * width + (x1 - x0)]};
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

    [[nodiscard]] Result<Sha256Digest> WindowDependency(
        const LunarTileKey key) const override {
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
    GdalRasterSource(
        RasterConfiguration configuration,
        DatasetArtifact dataset,
        DatasetPtr handle,
        std::filesystem::path raster_path,
        std::string driver_name)
        : configuration_(std::move(configuration)),
          dataset_(std::move(dataset)),
          dataset_handle_(std::move(handle)),
          raster_path_(std::move(raster_path)) {
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
};

class MosaicRasterSource final : public IRasterSource {
public:
    static Result<std::unique_ptr<IRasterSource>> Open(
        const RasterConfiguration& configuration,
        DatasetArtifact dataset) {
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
            auto opened = GdalRasterSource::Open(physical, dataset);
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

[[nodiscard]] Result<std::unique_ptr<IRasterSource>> open_logical_raster(
    const RasterConfiguration& configuration,
    DatasetArtifact dataset) {
    if (configuration.raster_files.size() <= 1U) {
        return GdalRasterSource::Open(configuration, std::move(dataset));
    }
    return MosaicRasterSource::Open(configuration, std::move(dataset));
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
    const ConfigurationIdentity& identity) {
    if (configuration.source_kind != BuilderSourceKind::raster || configuration.rasters.size() != 1) {
        return failure<std::unique_ptr<IRasterSource>>(
            ErrorCode::invalid_argument, "configuration does not select a raster source");
    }
    auto members = catalog_artifacts(configuration.rasters.front());
    if (!members) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(members).error());
    }
    auto bundle = artifact_bundle_identity(members.value(), configuration.rasters.front());
    if (!bundle) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(bundle).error());
    }
    auto dataset = make_dataset_artifact(
        configuration.rasters.front(),
        identity.dataset_ids.front(),
        std::move(members).value(),
        bundle.value().first,
        bundle.value().second);
    if (!dataset) {
        return Result<std::unique_ptr<IRasterSource>>::failure(std::move(dataset).error());
    }
    return open_logical_raster(configuration.rasters.front(), std::move(dataset).value());
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

Result<std::vector<std::unique_ptr<IRasterSource>>> open_raster_sources(
    const BuilderConfiguration& configuration,
    const ConfigurationIdentity& identity) {
    if (configuration.source_kind != BuilderSourceKind::raster ||
        configuration.rasters.empty() ||
        configuration.rasters.size() != identity.dataset_ids.size()) {
        return failure<std::vector<std::unique_ptr<IRasterSource>>>(
            ErrorCode::invalid_argument, "configuration does not select valid raster sources");
    }
    std::vector<std::unique_ptr<IRasterSource>> sources;
    sources.reserve(configuration.rasters.size());
    for (std::size_t index = 0; index < configuration.rasters.size(); ++index) {
        const RasterConfiguration& raster = configuration.rasters[index];
        auto members = catalog_artifacts(raster);
        if (!members) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(members).error());
        }
        auto bundle = artifact_bundle_identity(members.value(), raster);
        if (!bundle) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(bundle).error());
        }
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
        auto source = open_logical_raster(raster, std::move(dataset).value());
        if (!source) {
            return Result<std::vector<std::unique_ptr<IRasterSource>>>::failure(
                std::move(source).error());
        }
        sources.push_back(std::move(source).value());
    }
    return Result<std::vector<std::unique_ptr<IRasterSource>>>::success(std::move(sources));
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
