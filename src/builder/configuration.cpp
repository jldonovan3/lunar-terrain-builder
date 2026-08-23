#include "builder/configuration.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/integrity.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain::builder {
namespace {

using ByteVector = std::vector<std::byte>;

[[nodiscard]] Error configuration_error(
    const std::filesystem::path& path,
    std::string message) {
    return Error{ErrorCode::invalid_argument, std::move(message)}.with_path(path.string());
}

[[nodiscard]] bool contains(
    const std::initializer_list<std::string_view> allowed,
    const std::string_view value) noexcept {
    return std::ranges::find(allowed, value) != allowed.end();
}

[[nodiscard]] Result<void> validate_keys(
    const toml::table& table,
    const std::initializer_list<std::string_view> allowed,
    const std::filesystem::path& path,
    const std::string_view table_name) {
    for (const auto& [key, node] : table) {
        static_cast<void>(node);
        if (!contains(allowed, key.str())) {
            return Result<void>::failure(configuration_error(
                path,
                fmt::format("unknown configuration key '{}.{}'", table_name, key.str())));
        }
    }
    return Result<void>::success();
}

[[nodiscard]] Result<const toml::table*> optional_table(
    const toml::table& root,
    const std::string_view name,
    const std::filesystem::path& path) {
    const toml::node* node = root.get(name);
    if (node == nullptr) {
        return Result<const toml::table*>::success(nullptr);
    }
    const toml::table* table = node->as_table();
    if (table == nullptr) {
        return Result<const toml::table*>::failure(configuration_error(
            path, fmt::format("configuration key '{}' must be a table", name)));
    }
    return Result<const toml::table*>::success(table);
}

template <typename T>
[[nodiscard]] Result<T> optional_value(
    const toml::table* table,
    const std::string_view table_name,
    const std::string_view key,
    T default_value,
    const std::filesystem::path& path) {
    if (table == nullptr || !table->contains(key)) {
        return Result<T>::success(std::move(default_value));
    }
    auto value = (*table)[key].template value<T>();
    if (!value) {
        return Result<T>::failure(configuration_error(
            path, fmt::format("configuration key '{}.{}' has the wrong type", table_name, key)));
    }
    return Result<T>::success(std::move(*value));
}

template <typename T>
[[nodiscard]] Result<T> required_value(
    const toml::table* table,
    const std::string_view table_name,
    const std::string_view key,
    const std::filesystem::path& path) {
    if (table == nullptr || !table->contains(key)) {
        return Result<T>::failure(configuration_error(
            path, fmt::format("configuration key '{}.{}' is required", table_name, key)));
    }
    auto value = (*table)[key].template value<T>();
    if (!value) {
        return Result<T>::failure(configuration_error(
            path, fmt::format("configuration key '{}.{}' has the wrong type", table_name, key)));
    }
    return Result<T>::success(std::move(*value));
}

[[nodiscard]] bool portable_name(const std::string_view name) noexcept {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    return std::ranges::all_of(name, [](const char character) {
        const auto value = static_cast<unsigned char>(character);
        return std::isalnum(value) != 0 || character == '-' || character == '_' || character == '.';
    });
}

[[nodiscard]] bool stable_key(const std::string_view key) noexcept {
    if (key.empty()) {
        return false;
    }
    return std::ranges::all_of(key, [](const char character) {
        const auto value = static_cast<unsigned char>(character);
        return (value >= static_cast<unsigned char>('a') && value <= static_cast<unsigned char>('z')) ||
               (value >= static_cast<unsigned char>('0') && value <= static_cast<unsigned char>('9')) ||
               character == '.' || character == '-' || character == '_';
    });
}

[[nodiscard]] bool source_relative_path(const std::string_view value) noexcept {
    if (value.empty() || value.front() == '/' || value.front() == '\\' ||
        value.find('\\') != std::string_view::npos || value.find('\0') != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t end = value.find('/', begin);
        const std::string_view part = value.substr(
            begin, end == std::string_view::npos ? value.size() - begin : end - begin);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string_view::npos) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1U;
    }
    return true;
}

[[nodiscard]] bool unsigned_utf8_less(
    const ArtifactMemberConfiguration& left,
    const ArtifactMemberConfiguration& right) noexcept {
    return std::lexicographical_compare(
        left.name.begin(), left.name.end(), right.name.begin(), right.name.end(),
        [](const char a, const char b) {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
        });
}

[[nodiscard]] Result<void> require_equal(
    const bool condition,
    const std::filesystem::path& path,
    std::string message) {
    if (!condition) {
        return Result<void>::failure(configuration_error(path, std::move(message)));
    }
    return Result<void>::success();
}

[[nodiscard]] std::optional<std::string> environment_value(const std::string& name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t bytes = 0;
    if (_dupenv_s(&value, &bytes, name.c_str()) != 0 || value == nullptr) {
        return std::nullopt;
    }
    std::string result{value};
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name.c_str());
    return value == nullptr ? std::nullopt : std::optional<std::string>{value};
#endif
}

void append_u64(ByteVector& bytes, const std::uint64_t value) {
    for (std::uint32_t index = 0; index < 8U; ++index) {
        bytes.push_back(static_cast<std::byte>(value >> (index * 8U)));
    }
}

void append_domain(ByteVector& bytes, const std::string_view domain) {
    const auto characters = std::as_bytes(std::span{domain});
    bytes.insert(bytes.end(), characters.begin(), characters.end());
    bytes.push_back(std::byte{0});
}

[[nodiscard]] Result<Sha256Digest> framed_hash(
    const std::string_view domain,
    const std::string_view text) {
    ByteVector bytes;
    bytes.reserve(domain.size() + 1U + 8U + text.size());
    append_domain(bytes, domain);
    append_u64(bytes, text.size());
    const auto text_bytes = std::as_bytes(std::span{text});
    bytes.insert(bytes.end(), text_bytes.begin(), text_bytes.end());
    return sha256(bytes);
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

[[nodiscard]] std::string optional_u64_json(const std::optional<std::uint64_t> value) {
    return value ? std::to_string(*value) : "null";
}

[[nodiscard]] std::string optional_digest_json(const std::optional<Sha256Digest>& value) {
    return value ? json_string(value->to_hex()) : "null";
}

[[nodiscard]] std::string canonical_artifact_members_json(
    const std::vector<ArtifactMemberConfiguration>& members) {
    std::string result;
    for (std::size_t index = 0; index < members.size(); ++index) {
        if (index != 0) {
            result.push_back(',');
        }
        result += fmt::format(
            "{{\"bytes\":{},\"name\":{},\"sha256\":{}}}",
            optional_u64_json(members[index].expected_bytes),
            json_string(members[index].name),
            optional_digest_json(members[index].expected_sha256));
    }
    return result;
}

[[nodiscard]] std::string canonical_semantic_json(const BuilderConfiguration& configuration) {
    if (configuration.source_kind == BuilderSourceKind::synthetic) {
        return fmt::format(
            "{{\"algorithm_version\":1,\"datasets\":[{{\"amplitude_meters\":{},"
            "\"source_uri\":{},\"stable_key\":{}}}],\"datum\":{{\"elevation_origin_m\":-16384,"
            "\"elevation_step_m\":0.5,\"reference_radius_m\":1737400}},"
            "\"fusion\":{{\"algorithm\":\"synthetic_analytic_v1\",\"version\":1}},"
            "\"projection\":{{\"id\":1,\"version\":1}},\"quantization\":{{\"id\":1}},"
            "\"tiles\":{{\"apron\":1,\"cells\":256,\"maximum_level\":0}}}}",
            configuration.synthetic_amplitude_meters,
            json_string(configuration.synthetic_source_uri),
            json_string(configuration.synthetic_stable_key));
    }

    const RasterConfiguration& raster = *configuration.raster;
    const std::string representation = raster.elevation_representation ==
        ElevationRepresentation::elevation_meters ? "elevation_meters" : "radius_meters";
    const std::string no_data_policy = raster.no_data_policy == NoDataPolicy::error
        ? "error" : "nearest_valid";
    return fmt::format(
        "{{\"algorithm_version\":1,\"datasets\":[{{\"artifact_bundle_bytes\":{},"
        "\"artifact_bundle_sha256\":{},\"artifact_members\":[{}],"
        "\"auxiliary_member\":{},\"bounds_degrees\":{{\"east\":{},\"north\":{},"
        "\"south\":{},\"west\":{}}},\"elevation_representation\":{},"
        "\"expected_data_type\":{},\"expected_height\":{},\"expected_width\":{},"
        "\"fusion_policy\":\"Replace\",\"instrument\":{},\"label_member\":{},"
        "\"license\":{},\"metadata_override\":{},\"mission\":{},\"no_data_policy\":{},"
        "\"nominal_resolution_m\":{},\"original_crs\":{},\"priority\":{},\"producer\":{},"
        "\"product_name\":{},\"product_version\":{},\"raster_member\":{},"
        "\"sample_offset\":{},\"sample_scale\":{},"
        "\"source_no_data\":{},\"source_reference_radius_m\":{},\"source_uri\":{},"
        "\"stable_key\":{}}}],\"datum\":{{\"elevation_origin_m\":-16384,"
        "\"elevation_step_m\":0.5,\"reference_radius_m\":1737400}},"
        "\"fusion\":{{\"algorithm\":\"Replace\",\"version\":1}},"
        "\"projection\":{{\"id\":1,\"version\":1}},\"quantization\":{{\"id\":1}},"
        "\"tiles\":{{\"apron\":1,\"cells\":256,\"maximum_level\":{}}}}}",
        optional_u64_json(raster.expected_bundle_bytes),
        optional_digest_json(raster.expected_bundle_sha256),
        canonical_artifact_members_json(raster.artifact_members),
        json_string(raster.auxiliary_member),
        raster.east_longitude_degrees,
        raster.north_latitude_degrees,
        raster.south_latitude_degrees,
        raster.west_longitude_degrees,
        json_string(representation),
        json_string(raster.expected_data_type),
        raster.expected_height,
        raster.expected_width,
        json_string(raster.instrument),
        json_string(raster.label_member),
        json_string(raster.license),
        raster.metadata_override,
        json_string(raster.mission),
        json_string(no_data_policy),
        raster.nominal_resolution_meters,
        json_string(raster.original_crs),
        raster.priority,
        json_string(raster.producer),
        json_string(raster.product_name),
        json_string(raster.product_version),
        json_string(raster.raster_member),
        raster.sample_offset,
        raster.sample_scale,
        raster.source_no_data,
        raster.source_reference_radius_meters,
        json_string(raster.source_uri),
        json_string(raster.stable_key),
        configuration.maximum_level);
}

[[nodiscard]] std::string canonical_builder_json(
    const BuilderConfiguration& configuration,
    const std::string_view semantic_json) {
    return fmt::format(
        "{{\"package\":{{\"codec\":\"zstd\",\"codec_level\":3,\"format_major\":1,"
        "\"format_minor\":0,\"name\":{},\"pack_naming_policy\":\"database_id_v1\","
        "\"target_pack_bytes\":{}}},\"semantic\":{}}}",
        json_string(configuration.database_name),
        configuration.target_pack_bytes,
        semantic_json);
}

[[nodiscard]] Result<DatasetId> make_dataset_id(const std::string_view key) {
    ByteVector bytes;
    append_domain(bytes, "LTDB_DATASET_V1");
    append_u64(bytes, key.size());
    const auto key_bytes = std::as_bytes(std::span{key});
    bytes.insert(bytes.end(), key_bytes.begin(), key_bytes.end());
    auto digest = sha256(bytes);
    if (!digest) {
        return Result<DatasetId>::failure(std::move(digest).error());
    }
    const auto& value = digest.value().bytes;
    const std::uint32_t id =
        std::to_integer<std::uint32_t>(value[0]) |
        (std::to_integer<std::uint32_t>(value[1]) << 8U) |
        (std::to_integer<std::uint32_t>(value[2]) << 16U) |
        (std::to_integer<std::uint32_t>(value[3]) << 24U);
    return Result<DatasetId>::success(DatasetId{id});
}

[[nodiscard]] Result<std::vector<ArtifactMemberConfiguration>> artifact_members(
    const toml::table* raster,
    const std::filesystem::path& path) {
    const toml::array* values = raster == nullptr ? nullptr : (*raster)["artifact_members"].as_array();
    if (values == nullptr || values->empty()) {
        return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
            path, "configuration key 'raster.artifact_members' must be a nonempty array"));
    }
    if (values->size() > std::numeric_limits<std::uint32_t>::max()) {
        return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
            path, "raster.artifact_members exceeds the v1 member-count range"));
    }
    std::vector<ArtifactMemberConfiguration> result;
    result.reserve(values->size());
    for (std::size_t index = 0; index < values->size(); ++index) {
        const toml::table* member = values->get(index)->as_table();
        if (member == nullptr) {
            return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
                path, "each raster.artifact_members entry must be an inline table"));
        }
        auto keys = validate_keys(*member, {"name", "bytes", "sha256"}, path, "raster.artifact_members");
        if (!keys) {
            return Result<std::vector<ArtifactMemberConfiguration>>::failure(std::move(keys).error());
        }
        auto name = required_value<std::string>(member, "raster.artifact_members", "name", path);
        if (!name) {
            return Result<std::vector<ArtifactMemberConfiguration>>::failure(std::move(name).error());
        }
        ArtifactMemberConfiguration parsed;
        parsed.name = std::move(name).value();
        if (member->contains("bytes")) {
            auto bytes = required_value<std::int64_t>(member, "raster.artifact_members", "bytes", path);
            if (!bytes || bytes.value() < 0) {
                return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
                    path, "raster artifact member bytes must be nonnegative"));
            }
            parsed.expected_bytes = static_cast<std::uint64_t>(bytes.value());
        }
        if (member->contains("sha256")) {
            auto digest_text = required_value<std::string>(
                member, "raster.artifact_members", "sha256", path);
            if (!digest_text) {
                return Result<std::vector<ArtifactMemberConfiguration>>::failure(
                    std::move(digest_text).error());
            }
            auto digest = Sha256Digest::from_hex(digest_text.value());
            if (!digest) {
                Error error = std::move(digest).error();
                error.with_path(path.string());
                return Result<std::vector<ArtifactMemberConfiguration>>::failure(std::move(error));
            }
            parsed.expected_sha256 = digest.value();
        }
        if (parsed.name.size() > std::numeric_limits<std::uint32_t>::max() ||
            !source_relative_path(parsed.name)) {
            return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
                path, "raster artifact member names must be portable source-relative paths"));
        }
        result.push_back(std::move(parsed));
    }
    std::ranges::sort(result, unsigned_utf8_less);
    if (std::ranges::adjacent_find(result, {}, &ArtifactMemberConfiguration::name) != result.end()) {
        return Result<std::vector<ArtifactMemberConfiguration>>::failure(configuration_error(
            path, "raster artifact member names must be unique"));
    }
    return Result<std::vector<ArtifactMemberConfiguration>>::success(std::move(result));
}

}  // namespace

Result<BuilderConfiguration> load_configuration(const std::filesystem::path& path) {
    toml::table root;
    try {
        root = toml::parse_file(path.string());
    } catch (const toml::parse_error& error) {
        return Result<BuilderConfiguration>::failure(Error{
            ErrorCode::parse_error,
            fmt::format("TOML parse failed: {}", error.description())}.with_path(path.string()));
    }

    auto top_keys = validate_keys(
        root,
        {"database", "datum", "projection", "tiles", "packaging", "synthetic", "raster", "local"},
        path,
        "root");
    if (!top_keys) {
        return Result<BuilderConfiguration>::failure(std::move(top_keys).error());
    }

    auto database = optional_table(root, "database", path);
    auto datum = optional_table(root, "datum", path);
    auto projection = optional_table(root, "projection", path);
    auto tiles = optional_table(root, "tiles", path);
    auto packaging = optional_table(root, "packaging", path);
    auto synthetic = optional_table(root, "synthetic", path);
    auto raster = optional_table(root, "raster", path);
    auto local = optional_table(root, "local", path);
    for (const auto* result : {&database, &datum, &projection, &tiles, &packaging, &synthetic, &raster, &local}) {
        if (!*result) {
            return Result<BuilderConfiguration>::failure(result->error());
        }
    }
    if (database.value() == nullptr) {
        return Result<BuilderConfiguration>::failure(
            configuration_error(path, "configuration table 'database' is required"));
    }
    if (synthetic.value() != nullptr && raster.value() != nullptr) {
        return Result<BuilderConfiguration>::failure(configuration_error(
            path, "configuration may contain either 'synthetic' or 'raster', not both"));
    }

    const std::array table_checks{
        validate_keys(*database.value(), {"name", "output_directory", "format_major", "format_minor"}, path, "database"),
        datum.value() == nullptr ? Result<void>::success() : validate_keys(*datum.value(), {"reference_radius_m", "elevation_origin_m", "elevation_step_m"}, path, "datum"),
        projection.value() == nullptr ? Result<void>::success() : validate_keys(*projection.value(), {"type", "version"}, path, "projection"),
        tiles.value() == nullptr ? Result<void>::success() : validate_keys(*tiles.value(), {"cells", "apron", "max_level"}, path, "tiles"),
        packaging.value() == nullptr ? Result<void>::success() : validate_keys(*packaging.value(), {"target_pack_bytes", "codec", "codec_level"}, path, "packaging"),
        synthetic.value() == nullptr ? Result<void>::success() : validate_keys(*synthetic.value(), {"stable_key", "source_uri", "amplitude_meters"}, path, "synthetic"),
        raster.value() == nullptr ? Result<void>::success() : validate_keys(
            *raster.value(),
            {"stable_key", "source_uri", "product_name", "producer", "mission", "instrument",
             "product_version", "original_crs", "license", "raster_member", "auxiliary_member",
             "label_member", "expected_data_type", "artifact_members", "artifact_bundle_bytes", "artifact_bundle_sha256",
             "expected_width", "expected_height", "west_longitude_degrees", "east_longitude_degrees",
             "south_latitude_degrees", "north_latitude_degrees", "nominal_resolution_meters",
             "source_no_data", "sample_scale", "sample_offset", "elevation_representation",
             "source_reference_radius_meters", "no_data_policy", "metadata_override", "priority"},
            path,
            "raster"),
        local.value() == nullptr ? Result<void>::success() : validate_keys(
            *local.value(), {"threads", "cache_directory", "source_root", "source_root_environment"}, path, "local"),
    };
    for (const auto& check : table_checks) {
        if (!check) {
            return Result<BuilderConfiguration>::failure(check.error());
        }
    }

    auto name = required_value<std::string>(database.value(), "database", "name", path);
    auto output = optional_value<std::string>(database.value(), "database", "output_directory", ".", path);
    auto major = optional_value<std::int64_t>(database.value(), "database", "format_major", 1, path);
    auto minor = optional_value<std::int64_t>(database.value(), "database", "format_minor", 0, path);
    auto radius = optional_value<double>(datum.value(), "datum", "reference_radius_m", 1'737'400.0, path);
    auto origin = optional_value<double>(datum.value(), "datum", "elevation_origin_m", -16'384.0, path);
    auto step = optional_value<double>(datum.value(), "datum", "elevation_step_m", 0.5, path);
    auto projection_type = optional_value<std::string>(projection.value(), "projection", "type", "qsc", path);
    auto projection_version = optional_value<std::int64_t>(projection.value(), "projection", "version", 1, path);
    auto cells = optional_value<std::int64_t>(tiles.value(), "tiles", "cells", 256, path);
    auto apron = optional_value<std::int64_t>(tiles.value(), "tiles", "apron", 1, path);
    auto maximum_level = optional_value<std::int64_t>(tiles.value(), "tiles", "max_level", 0, path);
    auto target_pack_bytes = optional_value<std::int64_t>(
        packaging.value(), "packaging", "target_pack_bytes", 1'073'741'824LL, path);
    auto codec = optional_value<std::string>(packaging.value(), "packaging", "codec", "zstd", path);
    auto codec_level = optional_value<std::int64_t>(packaging.value(), "packaging", "codec_level", 3, path);
    auto synthetic_key = optional_value<std::string>(
        synthetic.value(), "synthetic", "stable_key", "synthetic.p0.v1", path);
    auto synthetic_uri = optional_value<std::string>(
        synthetic.value(), "synthetic", "source_uri", "synthetic://analytic-v1", path);
    auto amplitude = optional_value<std::int64_t>(
        synthetic.value(), "synthetic", "amplitude_meters", 2'048, path);
    auto threads = optional_value<std::int64_t>(local.value(), "local", "threads", 1, path);
    auto cache = optional_value<std::string>(local.value(), "local", "cache_directory", ".ltbuild", path);

    if (!(name && output && major && minor && radius && origin && step && projection_type &&
          projection_version && cells && apron && maximum_level && target_pack_bytes && codec &&
          codec_level && synthetic_key && synthetic_uri && amplitude && threads && cache)) {
        const Error* first_error = nullptr;
        const auto capture = [&first_error](const auto& value) {
            if (!value && first_error == nullptr) {
                first_error = &value.error();
            }
        };
        capture(name); capture(output); capture(major); capture(minor); capture(radius); capture(origin);
        capture(step); capture(projection_type); capture(projection_version); capture(cells); capture(apron);
        capture(maximum_level); capture(target_pack_bytes); capture(codec); capture(codec_level);
        capture(synthetic_key); capture(synthetic_uri); capture(amplitude); capture(threads); capture(cache);
        return Result<BuilderConfiguration>::failure(*first_error);
    }

    const bool is_raster = raster.value() != nullptr;
    const std::array locked_checks{
        require_equal(portable_name(name.value()), path, "database.name must be a portable ASCII path component"),
        require_equal(major.value() == 1 && minor.value() == 0, path, "the builder writes only format version 1.0"),
        require_equal(radius.value() == 1'737'400.0 && origin.value() == -16'384.0 && step.value() == 0.5,
                      path, "the builder requires the frozen v1 datum and quantization values"),
        require_equal(projection_type.value() == "qsc" && projection_version.value() == 1,
                      path, "the builder requires QSC projection version 1"),
        require_equal(cells.value() == 256 && apron.value() == 1,
                      path, "the builder requires 256 cells and one apron sample"),
        require_equal(maximum_level.value() >= 0 && maximum_level.value() <= LunarTileKey::max_level,
                      path, "tiles.max_level must be in the range 0 through 28"),
        require_equal(is_raster || maximum_level.value() == 0,
                      path, "the synthetic P0 source emits only level-zero tiles"),
        require_equal(target_pack_bytes.value() > 0, path, "packaging.target_pack_bytes must be positive"),
        require_equal(codec.value() == "zstd" && codec_level.value() == 3,
                      path, "canonical packaging requires Zstandard level 3"),
        require_equal(threads.value() > 0 && threads.value() <= std::numeric_limits<std::uint32_t>::max(),
                      path, "local.threads is outside the supported range"),
    };
    for (const auto& check : locked_checks) {
        if (!check) {
            return Result<BuilderConfiguration>::failure(check.error());
        }
    }

    const std::filesystem::path base = path.has_parent_path() ? path.parent_path() : std::filesystem::path{"."};
    BuilderConfiguration configuration;
    configuration.source_path = path;
    configuration.output_directory = std::filesystem::path{output.value()};
    configuration.cache_directory = std::filesystem::path{cache.value()};
    if (configuration.output_directory.is_relative()) {
        configuration.output_directory = base / configuration.output_directory;
    }
    if (configuration.cache_directory.is_relative()) {
        configuration.cache_directory = base / configuration.cache_directory;
    }
    configuration.output_directory = configuration.output_directory.lexically_normal();
    configuration.cache_directory = configuration.cache_directory.lexically_normal();
    configuration.database_name = std::move(name).value();
    configuration.target_pack_bytes = static_cast<std::uint64_t>(target_pack_bytes.value());
    configuration.worker_threads = static_cast<std::uint32_t>(threads.value());
    configuration.maximum_level = static_cast<std::uint8_t>(maximum_level.value());

    if (!is_raster) {
        const std::array synthetic_checks{
            require_equal(stable_key(synthetic_key.value()), path,
                          "synthetic.stable_key must contain only lowercase ASCII letters, digits, '.', '-', or '_'"),
            require_equal(!synthetic_uri.value().empty() && synthetic_uri.value().find('\0') == std::string::npos,
                          path, "synthetic.source_uri must be nonempty and contain no NUL"),
            require_equal(amplitude.value() >= 0 && amplitude.value() <= 8'000,
                          path, "synthetic.amplitude_meters must be between 0 and 8000"),
        };
        for (const auto& check : synthetic_checks) {
            if (!check) {
                return Result<BuilderConfiguration>::failure(check.error());
            }
        }
        configuration.synthetic_stable_key = std::move(synthetic_key).value();
        configuration.synthetic_source_uri = std::move(synthetic_uri).value();
        configuration.synthetic_amplitude_meters = static_cast<std::int32_t>(amplitude.value());
        return Result<BuilderConfiguration>::success(std::move(configuration));
    }

    RasterConfiguration raster_configuration;
    auto raster_key = required_value<std::string>(raster.value(), "raster", "stable_key", path);
    auto raster_uri = required_value<std::string>(raster.value(), "raster", "source_uri", path);
    auto product_name = optional_value<std::string>(raster.value(), "raster", "product_name", "", path);
    auto producer = optional_value<std::string>(raster.value(), "raster", "producer", "", path);
    auto mission = optional_value<std::string>(raster.value(), "raster", "mission", "", path);
    auto instrument = optional_value<std::string>(raster.value(), "raster", "instrument", "", path);
    auto product_version = optional_value<std::string>(raster.value(), "raster", "product_version", "", path);
    auto original_crs = required_value<std::string>(raster.value(), "raster", "original_crs", path);
    auto license = optional_value<std::string>(raster.value(), "raster", "license", "", path);
    auto raster_member = required_value<std::string>(raster.value(), "raster", "raster_member", path);
    auto auxiliary_member = optional_value<std::string>(raster.value(), "raster", "auxiliary_member", "", path);
    auto label_member = optional_value<std::string>(raster.value(), "raster", "label_member", "", path);
    auto expected_data_type = required_value<std::string>(
        raster.value(), "raster", "expected_data_type", path);
    auto members = artifact_members(raster.value(), path);
    auto expected_width = required_value<std::int64_t>(raster.value(), "raster", "expected_width", path);
    auto expected_height = required_value<std::int64_t>(raster.value(), "raster", "expected_height", path);
    auto west = required_value<double>(raster.value(), "raster", "west_longitude_degrees", path);
    auto east = required_value<double>(raster.value(), "raster", "east_longitude_degrees", path);
    auto south = required_value<double>(raster.value(), "raster", "south_latitude_degrees", path);
    auto north = required_value<double>(raster.value(), "raster", "north_latitude_degrees", path);
    auto resolution = required_value<double>(raster.value(), "raster", "nominal_resolution_meters", path);
    auto no_data = required_value<double>(raster.value(), "raster", "source_no_data", path);
    auto sample_scale = required_value<double>(raster.value(), "raster", "sample_scale", path);
    auto sample_offset = optional_value<double>(raster.value(), "raster", "sample_offset", 0.0, path);
    auto representation = required_value<std::string>(raster.value(), "raster", "elevation_representation", path);
    auto source_radius = optional_value<double>(
        raster.value(), "raster", "source_reference_radius_meters", 1'737'400.0, path);
    auto no_data_policy = optional_value<std::string>(
        raster.value(), "raster", "no_data_policy", "error", path);
    auto metadata_override = optional_value<bool>(
        raster.value(), "raster", "metadata_override", false, path);
    auto priority = optional_value<std::int64_t>(raster.value(), "raster", "priority", 0, path);
    if (!(raster_key && raster_uri && product_name && producer && mission && instrument &&
          product_version && original_crs && license && raster_member && auxiliary_member &&
          label_member && expected_data_type && members && expected_width && expected_height && west && east && south &&
          north && resolution && no_data && sample_scale && sample_offset && representation &&
          source_radius && no_data_policy && metadata_override && priority)) {
        const Error* first_error = nullptr;
        const auto capture = [&first_error](const auto& value) {
            if (!value && first_error == nullptr) {
                first_error = &value.error();
            }
        };
        capture(raster_key); capture(raster_uri); capture(product_name); capture(producer);
        capture(mission); capture(instrument); capture(product_version); capture(original_crs);
        capture(license); capture(raster_member); capture(auxiliary_member); capture(label_member);
        capture(expected_data_type); capture(members); capture(expected_width); capture(expected_height); capture(west);
        capture(east); capture(south); capture(north); capture(resolution); capture(no_data);
        capture(sample_scale); capture(sample_offset); capture(representation); capture(source_radius);
        capture(no_data_policy); capture(metadata_override); capture(priority);
        return Result<BuilderConfiguration>::failure(*first_error);
    }

    std::optional<std::uint64_t> bundle_bytes;
    if (raster.value()->contains("artifact_bundle_bytes")) {
        auto value = required_value<std::int64_t>(raster.value(), "raster", "artifact_bundle_bytes", path);
        if (!value || value.value() < 0) {
            return Result<BuilderConfiguration>::failure(configuration_error(
                path, "raster.artifact_bundle_bytes must be nonnegative"));
        }
        bundle_bytes = static_cast<std::uint64_t>(value.value());
    }
    std::optional<Sha256Digest> bundle_hash;
    if (raster.value()->contains("artifact_bundle_sha256")) {
        auto value = required_value<std::string>(raster.value(), "raster", "artifact_bundle_sha256", path);
        if (!value) {
            return Result<BuilderConfiguration>::failure(std::move(value).error());
        }
        auto digest = Sha256Digest::from_hex(value.value());
        if (!digest) {
            Error error = std::move(digest).error();
            error.with_path(path.string());
            return Result<BuilderConfiguration>::failure(std::move(error));
        }
        bundle_hash = digest.value();
    }

    auto source_root = optional_value<std::string>(local.value(), "local", "source_root", "", path);
    auto source_root_environment = optional_value<std::string>(
        local.value(), "local", "source_root_environment", "", path);
    if (!(source_root && source_root_environment)) {
        return Result<BuilderConfiguration>::failure(
            source_root ? source_root_environment.error() : source_root.error());
    }
    if (!source_root.value().empty() && !source_root_environment.value().empty()) {
        return Result<BuilderConfiguration>::failure(configuration_error(
            path, "local.source_root and local.source_root_environment are mutually exclusive"));
    }
    std::filesystem::path resolved_root;
    if (!source_root_environment.value().empty()) {
        const auto resolved_environment = environment_value(source_root_environment.value());
        if (!resolved_environment || resolved_environment->empty()) {
            return Result<BuilderConfiguration>::failure(configuration_error(
                path,
                fmt::format("environment variable '{}' does not resolve the raster source root",
                            source_root_environment.value())));
        }
        resolved_root = std::filesystem::path{*resolved_environment};
    } else if (!source_root.value().empty()) {
        resolved_root = std::filesystem::path{source_root.value()};
        if (resolved_root.is_relative()) {
            resolved_root = base / resolved_root;
        }
    } else {
        return Result<BuilderConfiguration>::failure(configuration_error(
            path, "a raster source requires local.source_root or local.source_root_environment"));
    }

    const bool raster_member_present = std::ranges::any_of(
        members.value(), [&](const ArtifactMemberConfiguration& member) {
            return member.name == raster_member.value();
        });
    const bool auxiliary_present = auxiliary_member.value().empty() || std::ranges::any_of(
        members.value(), [&](const ArtifactMemberConfiguration& member) {
            return member.name == auxiliary_member.value();
        });
    const bool label_present = label_member.value().empty() || std::ranges::any_of(
        members.value(), [&](const ArtifactMemberConfiguration& member) {
            return member.name == label_member.value();
        });
    const std::array raster_checks{
        require_equal(stable_key(raster_key.value()), path,
                      "raster.stable_key must contain only lowercase ASCII letters, digits, '.', '-', or '_'"),
        require_equal(!raster_uri.value().empty() && raster_uri.value().find('\0') == std::string::npos,
                      path, "raster.source_uri must be nonempty and contain no NUL"),
        require_equal(source_relative_path(raster_member.value()), path,
                      "raster.raster_member must be a portable source-relative path"),
        require_equal(auxiliary_member.value().empty() || source_relative_path(auxiliary_member.value()),
                      path, "raster.auxiliary_member must be empty or a portable source-relative path"),
        require_equal(label_member.value().empty() || source_relative_path(label_member.value()),
                      path, "raster.label_member must be empty or a portable source-relative path"),
        require_equal(raster_member_present && auxiliary_present && label_present, path,
                      "raster and associated sidecar members must appear in raster.artifact_members"),
        require_equal(!expected_data_type.value().empty() &&
                          expected_data_type.value().find('\0') == std::string::npos,
                      path, "raster.expected_data_type must be nonempty"),
        require_equal(expected_width.value() > 0 && expected_height.value() > 0 &&
                          expected_width.value() <= std::numeric_limits<std::uint32_t>::max() &&
                          expected_height.value() <= std::numeric_limits<std::uint32_t>::max(),
                      path, "raster expected dimensions are outside the supported range"),
        require_equal(std::isfinite(west.value()) && std::isfinite(east.value()) &&
                          std::isfinite(south.value()) && std::isfinite(north.value()) &&
                          west.value() < east.value() && south.value() < north.value() &&
                          west.value() >= -180.0 && east.value() <= 360.0 &&
                          south.value() >= -90.0 && north.value() <= 90.0,
                      path, "raster geographic bounds are invalid or wrap longitude"),
        require_equal(std::isfinite(resolution.value()) && resolution.value() > 0.0 &&
                          resolution.value() * 1'000.0 <=
                              static_cast<double>(std::numeric_limits<std::uint32_t>::max()),
                      path, "raster.nominal_resolution_meters is outside the v1 millimeter range"),
        require_equal(std::isfinite(no_data.value()) && std::isfinite(sample_scale.value()) &&
                          sample_scale.value() != 0.0 && std::isfinite(sample_offset.value()) &&
                          std::isfinite(source_radius.value()) && source_radius.value() > 0.0,
                      path, "raster sample, no-data, or datum values are invalid"),
        require_equal(representation.value() == "elevation_meters" || representation.value() == "radius_meters",
                      path, "raster.elevation_representation must be 'elevation_meters' or 'radius_meters'"),
        require_equal(no_data_policy.value() == "error" || no_data_policy.value() == "nearest_valid",
                      path, "raster.no_data_policy must be 'error' or 'nearest_valid'"),
        require_equal(priority.value() >= std::numeric_limits<std::int32_t>::min() &&
                          priority.value() <= std::numeric_limits<std::int32_t>::max(),
                      path, "raster.priority is outside the supported range"),
    };
    for (const auto& check : raster_checks) {
        if (!check) {
            return Result<BuilderConfiguration>::failure(check.error());
        }
    }

    raster_configuration.stable_key = std::move(raster_key).value();
    raster_configuration.source_uri = std::move(raster_uri).value();
    raster_configuration.product_name = std::move(product_name).value();
    raster_configuration.producer = std::move(producer).value();
    raster_configuration.mission = std::move(mission).value();
    raster_configuration.instrument = std::move(instrument).value();
    raster_configuration.product_version = std::move(product_version).value();
    raster_configuration.original_crs = std::move(original_crs).value();
    raster_configuration.license = std::move(license).value();
    raster_configuration.raster_member = std::move(raster_member).value();
    raster_configuration.auxiliary_member = std::move(auxiliary_member).value();
    raster_configuration.label_member = std::move(label_member).value();
    raster_configuration.expected_data_type = std::move(expected_data_type).value();
    raster_configuration.artifact_members = std::move(members).value();
    raster_configuration.expected_bundle_bytes = bundle_bytes;
    raster_configuration.expected_bundle_sha256 = bundle_hash;
    raster_configuration.source_root = resolved_root.lexically_normal();
    raster_configuration.expected_width = static_cast<std::uint32_t>(expected_width.value());
    raster_configuration.expected_height = static_cast<std::uint32_t>(expected_height.value());
    raster_configuration.west_longitude_degrees = west.value();
    raster_configuration.east_longitude_degrees = east.value();
    raster_configuration.south_latitude_degrees = south.value();
    raster_configuration.north_latitude_degrees = north.value();
    raster_configuration.nominal_resolution_meters = resolution.value();
    raster_configuration.source_no_data = no_data.value();
    raster_configuration.sample_scale = sample_scale.value();
    raster_configuration.sample_offset = sample_offset.value();
    raster_configuration.source_reference_radius_meters = source_radius.value();
    raster_configuration.priority = static_cast<std::int32_t>(priority.value());
    raster_configuration.elevation_representation = representation.value() == "elevation_meters"
        ? ElevationRepresentation::elevation_meters : ElevationRepresentation::radius_meters;
    raster_configuration.no_data_policy = no_data_policy.value() == "error"
        ? NoDataPolicy::error : NoDataPolicy::nearest_valid;
    raster_configuration.metadata_override = metadata_override.value();
    configuration.source_kind = BuilderSourceKind::raster;
    configuration.raster = std::move(raster_configuration);
    return Result<BuilderConfiguration>::success(std::move(configuration));
}

Result<ConfigurationIdentity> identify_configuration(
    const BuilderConfiguration& configuration) {
    const std::string semantic = canonical_semantic_json(configuration);
    const std::string builder = canonical_builder_json(configuration, semantic);
    auto builder_hash = framed_hash("LTDB_BUILDER_CONFIG_V1", builder);
    if (!builder_hash) {
        return Result<ConfigurationIdentity>::failure(std::move(builder_hash).error());
    }
    auto semantic_hash = framed_hash("LTDB_SEMANTIC_CONFIG_V1", semantic);
    if (!semantic_hash) {
        return Result<ConfigurationIdentity>::failure(std::move(semantic_hash).error());
    }
    const std::string_view key = configuration.source_kind == BuilderSourceKind::synthetic
        ? std::string_view{configuration.synthetic_stable_key}
        : std::string_view{configuration.raster->stable_key};
    auto dataset_id = make_dataset_id(key);
    if (!dataset_id) {
        return Result<ConfigurationIdentity>::failure(std::move(dataset_id).error());
    }
    return Result<ConfigurationIdentity>::success(ConfigurationIdentity{
        builder,
        semantic,
        builder_hash.value(),
        semantic_hash.value(),
        dataset_id.value(),
    });
}

}  // namespace lunar::terrain::builder
