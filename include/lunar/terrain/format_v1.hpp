#pragma once

#include <cstddef>
#include <cstdint>

namespace lunar::terrain::format_v1 {

inline constexpr std::uint16_t major_version = 1;
inline constexpr std::uint16_t minor_version = 0;
inline constexpr std::uint32_t endian_tag = 0x01020304U;
inline constexpr std::uint16_t tile_cells = 256;
inline constexpr std::uint16_t core_vertices = 257;
inline constexpr std::uint8_t apron_samples = 1;
inline constexpr std::uint16_t serialized_elevation_samples = 259;
inline constexpr std::uint16_t auxiliary_map_samples = 64;

namespace bytes {
inline constexpr std::size_t ltdb_header = 256;
inline constexpr std::size_t chunk_directory_entry = 40;
inline constexpr std::size_t dataset_record = 128;
inline constexpr std::size_t pack_record = 80;
inline constexpr std::size_t tile_index_record = 80;
inline constexpr std::size_t pack_header = 64;
inline constexpr std::size_t tile_header = 128;
inline constexpr std::size_t channel_record = 40;
inline constexpr std::size_t provenance_header = 16;
inline constexpr std::size_t provenance_palette_entry = 16;
}  // namespace bytes

namespace ltdb_header_offset {
inline constexpr std::size_t magic = 0;
inline constexpr std::size_t major = 4;
inline constexpr std::size_t minor = 6;
inline constexpr std::size_t header_bytes = 8;
inline constexpr std::size_t endian = 12;
inline constexpr std::size_t flags = 16;
inline constexpr std::size_t chunk_count = 20;
inline constexpr std::size_t chunk_directory = 24;
inline constexpr std::size_t database_id = 32;
inline constexpr std::size_t reference_radius = 48;
inline constexpr std::size_t elevation_origin = 56;
inline constexpr std::size_t elevation_step = 64;
inline constexpr std::size_t tile_cells = 72;
inline constexpr std::size_t core_vertices = 74;
inline constexpr std::size_t apron = 76;
inline constexpr std::size_t maximum_level = 77;
inline constexpr std::size_t projection = 78;
inline constexpr std::size_t quantization = 79;
inline constexpr std::size_t tile_count = 80;
inline constexpr std::size_t dataset_count = 88;
inline constexpr std::size_t pack_count = 92;
inline constexpr std::size_t builder_configuration_hash = 96;
inline constexpr std::size_t dataset_registry_hash = 128;
inline constexpr std::size_t tile_index_hash = 160;
inline constexpr std::size_t database_content_hash = 192;
inline constexpr std::size_t reserved = 224;
}  // namespace ltdb_header_offset

namespace tile_header_offset {
inline constexpr std::size_t magic = 0;
inline constexpr std::size_t version = 4;
inline constexpr std::size_t header_bytes = 6;
inline constexpr std::size_t tile_key = 8;
inline constexpr std::size_t flags = 16;
inline constexpr std::size_t channel_count = 20;
inline constexpr std::size_t tile_cells = 22;
inline constexpr std::size_t core_vertices = 24;
inline constexpr std::size_t apron = 26;
inline constexpr std::size_t encoding_profile = 27;
inline constexpr std::size_t effective_resolution = 28;
inline constexpr std::size_t geometric_error = 32;
inline constexpr std::size_t minimum_elevation = 36;
inline constexpr std::size_t maximum_elevation = 40;
inline constexpr std::size_t primary_dataset = 44;
inline constexpr std::size_t provenance_palette_count = 48;
inline constexpr std::size_t reserved0 = 50;
inline constexpr std::size_t channel_directory_offset = 52;
inline constexpr std::size_t channel_directory_bytes = 56;
inline constexpr std::size_t data_region_offset = 60;
inline constexpr std::size_t data_region_bytes = 64;
inline constexpr std::size_t dependency_hash = 68;
inline constexpr std::size_t content_hash = 84;
inline constexpr std::size_t reserved1 = 100;
}  // namespace tile_header_offset

namespace chunk_directory_offset {
inline constexpr std::size_t tag = 0;
inline constexpr std::size_t version = 4;
inline constexpr std::size_t flags = 6;
inline constexpr std::size_t file_offset = 8;
inline constexpr std::size_t stored_bytes = 16;
inline constexpr std::size_t logical_bytes = 24;
inline constexpr std::size_t crc32c = 32;
inline constexpr std::size_t reserved = 36;
}  // namespace chunk_directory_offset

namespace dataset_record_offset {
inline constexpr std::size_t dataset_id = 0;
inline constexpr std::size_t flags = 4;
inline constexpr std::size_t first_string_id = 8;
inline constexpr std::size_t nominal_resolution = 40;
inline constexpr std::size_t horizontal_accuracy = 48;
inline constexpr std::size_t vertical_accuracy = 56;
inline constexpr std::size_t source_no_data = 64;
inline constexpr std::size_t artifact_bundle_bytes = 72;
inline constexpr std::size_t artifact_bundle_hash = 80;
inline constexpr std::size_t meta_offset = 112;
inline constexpr std::size_t meta_bytes = 116;
inline constexpr std::size_t quality_schema = 120;
inline constexpr std::size_t reserved = 124;
}  // namespace dataset_record_offset

namespace pack_record_offset {
inline constexpr std::size_t pack_id = 0;
inline constexpr std::size_t flags = 4;
inline constexpr std::size_t path_string_id = 8;
inline constexpr std::size_t default_codec = 12;
inline constexpr std::size_t reserved = 14;
inline constexpr std::size_t tile_count = 16;
inline constexpr std::size_t file_bytes = 24;
inline constexpr std::size_t first_tile_key = 32;
inline constexpr std::size_t last_tile_key = 40;
inline constexpr std::size_t sha256 = 48;
}  // namespace pack_record_offset

namespace tile_index_offset {
inline constexpr std::size_t tile_key = 0;
inline constexpr std::size_t pack_id = 8;
inline constexpr std::size_t flags = 12;
inline constexpr std::size_t payload_offset = 16;
inline constexpr std::size_t stored_bytes = 24;
inline constexpr std::size_t logical_bytes = 28;
inline constexpr std::size_t minimum_elevation = 32;
inline constexpr std::size_t maximum_elevation = 34;
inline constexpr std::size_t primary_dataset = 36;
inline constexpr std::size_t effective_resolution = 40;
inline constexpr std::size_t geometric_error = 44;
inline constexpr std::size_t child_mask = 48;
inline constexpr std::size_t channel_count = 49;
inline constexpr std::size_t reserved = 50;
inline constexpr std::size_t payload_crc32c = 52;
inline constexpr std::size_t content_hash = 56;
inline constexpr std::size_t dependency_hash = 72;
}  // namespace tile_index_offset

namespace pack_header_offset {
inline constexpr std::size_t magic = 0;
inline constexpr std::size_t major = 4;
inline constexpr std::size_t minor = 6;
inline constexpr std::size_t header_bytes = 8;
inline constexpr std::size_t endian = 12;
inline constexpr std::size_t pack_id = 16;
inline constexpr std::size_t flags = 20;
inline constexpr std::size_t tile_count = 24;
inline constexpr std::size_t payload_region_offset = 32;
inline constexpr std::size_t file_bytes = 40;
inline constexpr std::size_t sha256_prefix = 48;
}  // namespace pack_header_offset

namespace channel_record_offset {
inline constexpr std::size_t channel_id = 0;
inline constexpr std::size_t version = 2;
inline constexpr std::size_t element_type = 4;
inline constexpr std::size_t components = 5;
inline constexpr std::size_t codec = 6;
inline constexpr std::size_t predictor = 7;
inline constexpr std::size_t width = 8;
inline constexpr std::size_t height = 10;
inline constexpr std::size_t flags = 12;
inline constexpr std::size_t data_offset = 16;
inline constexpr std::size_t stored_bytes = 20;
inline constexpr std::size_t logical_bytes = 24;
inline constexpr std::size_t crc32c = 28;
inline constexpr std::size_t parameter0 = 32;
inline constexpr std::size_t parameter1 = 36;
}  // namespace channel_record_offset

namespace provenance_header_offset {
inline constexpr std::size_t version = 0;
inline constexpr std::size_t palette_count = 2;
inline constexpr std::size_t map_width = 4;
inline constexpr std::size_t map_height = 6;
inline constexpr std::size_t index_width = 8;
inline constexpr std::size_t reserved0 = 9;
inline constexpr std::size_t flags = 10;
inline constexpr std::size_t reserved1 = 12;
}  // namespace provenance_header_offset

static_assert(ltdb_header_offset::reserved + 32 == bytes::ltdb_header);
static_assert(tile_header_offset::reserved1 + 28 == bytes::tile_header);
static_assert(chunk_directory_offset::reserved + 4 == bytes::chunk_directory_entry);
static_assert(dataset_record_offset::reserved + 4 == bytes::dataset_record);
static_assert(pack_record_offset::sha256 + 32 == bytes::pack_record);
static_assert(tile_index_offset::dependency_hash + 8 == bytes::tile_index_record);
static_assert(pack_header_offset::sha256_prefix + 16 == bytes::pack_header);
static_assert(channel_record_offset::parameter1 + 4 == bytes::channel_record);
static_assert(provenance_header_offset::reserved1 + 4 == bytes::provenance_header);
static_assert(4 + 2 + 2 + 8 + 8 + 8 + 4 + 4 == bytes::chunk_directory_entry);
static_assert(10 * 4 + 4 * 8 + 8 + 32 + 4 * 4 == bytes::dataset_record);
static_assert(3 * 4 + 2 * 2 + 4 * 8 + 32 == bytes::pack_record);
static_assert(8 + 2 * 4 + 8 + 2 * 4 + 2 * 2 + 3 * 4 + 2 + 2 + 4 + 16 + 8 == bytes::tile_index_record);
static_assert(4 + 2 * 2 + 4 * 4 + 3 * 8 + 16 == bytes::pack_header);
static_assert(2 * 2 + 4 * 1 + 2 * 2 + 7 * 4 == bytes::channel_record);
static_assert(5 * 2 + 2 * 1 + 4 == bytes::provenance_header);
static_assert(2 * 4 + 2 * 4 == bytes::provenance_palette_entry);

}  // namespace lunar::terrain::format_v1
