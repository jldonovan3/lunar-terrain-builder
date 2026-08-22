#include "builder/configuration.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <toml++/toml.hpp>

#include <lunar/terrain/error.hpp>
#include <lunar/terrain/integrity.hpp>

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

[[nodiscard]] Result<std::string> required_string(
    const toml::table* table,
    const std::string_view table_name,
    const std::string_view key,
    const std::filesystem::path& path) {
    if (table == nullptr || !table->contains(key)) {
        return Result<std::string>::failure(configuration_error(
            path, fmt::format("configuration key '{}.{}' is required", table_name, key)));
    }
    auto value = (*table)[key].value<std::string>();
    if (!value) {
        return Result<std::string>::failure(configuration_error(
            path, fmt::format("configuration key '{}.{}' must be a string", table_name, key)));
    }
    return Result<std::string>::success(std::move(*value));
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

[[nodiscard]] Result<void> require_equal(
    const bool condition,
    const std::filesystem::path& path,
    std::string message) {
    if (!condition) {
        return Result<void>::failure(configuration_error(path, std::move(message)));
    }
    return Result<void>::success();
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
            case '"':
                encoded += "\\\"";
                break;
            case '\\':
                encoded += "\\\\";
                break;
            case '\b':
                encoded += "\\b";
                break;
            case '\t':
                encoded += "\\t";
                break;
            case '\n':
                encoded += "\\n";
                break;
            case '\f':
                encoded += "\\f";
                break;
            case '\r':
                encoded += "\\r";
                break;
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

[[nodiscard]] std::string canonical_semantic_json(const BuilderConfiguration& configuration) {
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
        {"database", "datum", "projection", "tiles", "packaging", "synthetic", "local"},
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
    auto local = optional_table(root, "local", path);
    for (const auto* result : {&database, &datum, &projection, &tiles, &packaging, &synthetic, &local}) {
        if (!*result) {
            return Result<BuilderConfiguration>::failure(result->error());
        }
    }
    if (database.value() == nullptr) {
        return Result<BuilderConfiguration>::failure(
            configuration_error(path, "configuration table 'database' is required"));
    }

    const std::array table_checks{
        validate_keys(*database.value(), {"name", "output_directory", "format_major", "format_minor"}, path, "database"),
        datum.value() == nullptr ? Result<void>::success() : validate_keys(*datum.value(), {"reference_radius_m", "elevation_origin_m", "elevation_step_m"}, path, "datum"),
        projection.value() == nullptr ? Result<void>::success() : validate_keys(*projection.value(), {"type", "version"}, path, "projection"),
        tiles.value() == nullptr ? Result<void>::success() : validate_keys(*tiles.value(), {"cells", "apron", "max_level"}, path, "tiles"),
        packaging.value() == nullptr ? Result<void>::success() : validate_keys(*packaging.value(), {"target_pack_bytes", "codec", "codec_level"}, path, "packaging"),
        synthetic.value() == nullptr ? Result<void>::success() : validate_keys(*synthetic.value(), {"stable_key", "source_uri", "amplitude_meters"}, path, "synthetic"),
        local.value() == nullptr ? Result<void>::success() : validate_keys(*local.value(), {"threads", "cache_directory"}, path, "local"),
    };
    for (const auto& check : table_checks) {
        if (!check) {
            return Result<BuilderConfiguration>::failure(check.error());
        }
    }

    auto name = required_string(database.value(), "database", "name", path);
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
    auto source_uri = optional_value<std::string>(
        synthetic.value(), "synthetic", "source_uri", "synthetic://analytic-v1", path);
    auto amplitude = optional_value<std::int64_t>(
        synthetic.value(), "synthetic", "amplitude_meters", 2'048, path);
    auto threads = optional_value<std::int64_t>(local.value(), "local", "threads", 1, path);
    auto cache = optional_value<std::string>(
        local.value(), "local", "cache_directory", ".ltbuild", path);

    const bool values_parsed = name && output && major && minor && radius && origin && step &&
                               projection_type && projection_version && cells && apron &&
                               maximum_level && target_pack_bytes && codec && codec_level &&
                               synthetic_key && source_uri && amplitude && threads && cache;
    if (!values_parsed) {
        const Error* first_error = nullptr;
        const auto capture = [&first_error](const auto& value) {
            if (!value && first_error == nullptr) {
                first_error = &value.error();
            }
        };
        capture(name); capture(output); capture(major); capture(minor); capture(radius); capture(origin);
        capture(step); capture(projection_type); capture(projection_version); capture(cells); capture(apron);
        capture(maximum_level); capture(target_pack_bytes); capture(codec); capture(codec_level);
        capture(synthetic_key); capture(source_uri); capture(amplitude); capture(threads); capture(cache);
        return Result<BuilderConfiguration>::failure(*first_error);
    }

    const std::array locked_checks{
        require_equal(portable_name(name.value()), path, "database.name must be a portable ASCII path component"),
        require_equal(major.value() == 1 && minor.value() == 0, path, "M2 writes only format version 1.0"),
        require_equal(radius.value() == 1'737'400.0 && origin.value() == -16'384.0 && step.value() == 0.5,
                      path, "M2 requires the frozen v1 datum and quantization values"),
        require_equal(projection_type.value() == "qsc" && projection_version.value() == 1,
                      path, "M2 requires QSC projection version 1"),
        require_equal(cells.value() == 256 && apron.value() == 1 && maximum_level.value() == 0,
                      path, "M2 emits exactly six level-zero faces with 256 cells and one apron sample"),
        require_equal(target_pack_bytes.value() > 0, path, "packaging.target_pack_bytes must be positive"),
        require_equal(codec.value() == "zstd" && codec_level.value() == 3,
                      path, "M2 canonical packaging requires Zstandard level 3"),
        require_equal(stable_key(synthetic_key.value()), path,
                      "synthetic.stable_key must contain only lowercase ASCII letters, digits, '.', '-', or '_'"),
        require_equal(!source_uri.value().empty() && source_uri.value().find('\0') == std::string::npos,
                      path, "synthetic.source_uri must be nonempty and contain no NUL"),
        require_equal(amplitude.value() >= 0 && amplitude.value() <= 8'000,
                      path, "synthetic.amplitude_meters must be between 0 and 8000"),
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
    configuration.synthetic_stable_key = std::move(synthetic_key).value();
    configuration.synthetic_source_uri = std::move(source_uri).value();
    configuration.target_pack_bytes = static_cast<std::uint64_t>(target_pack_bytes.value());
    configuration.worker_threads = static_cast<std::uint32_t>(threads.value());
    configuration.synthetic_amplitude_meters = static_cast<std::int32_t>(amplitude.value());
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
    auto dataset_id = make_dataset_id(configuration.synthetic_stable_key);
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
