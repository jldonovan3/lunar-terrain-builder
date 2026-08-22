#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/result.hpp>

namespace lunar::terrain::builder {

struct BuilderConfiguration {
    std::filesystem::path source_path;
    std::filesystem::path output_directory;
    std::filesystem::path cache_directory;
    std::string database_name;
    std::string synthetic_stable_key;
    std::string synthetic_source_uri;
    std::uint64_t target_pack_bytes{1'073'741'824ULL};
    std::uint32_t worker_threads{1};
    std::int32_t synthetic_amplitude_meters{2'048};
};

struct ConfigurationIdentity {
    std::string canonical_builder_json;
    std::string canonical_semantic_json;
    Sha256Digest builder_hash;
    Sha256Digest semantic_hash;
    DatasetId synthetic_dataset_id;
};

[[nodiscard]] Result<BuilderConfiguration> load_configuration(
    const std::filesystem::path& path);
[[nodiscard]] Result<ConfigurationIdentity> identify_configuration(
    const BuilderConfiguration& configuration);

}  // namespace lunar::terrain::builder
