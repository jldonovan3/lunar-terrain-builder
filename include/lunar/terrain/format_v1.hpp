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

static_assert(ltdb_header_offset::reserved + 32 == bytes::ltdb_header);
static_assert(tile_header_offset::reserved1 + 28 == bytes::tile_header);
static_assert(4 + 2 + 2 + 8 + 8 + 8 + 4 + 4 == bytes::chunk_directory_entry);
static_assert(10 * 4 + 4 * 8 + 8 + 32 + 4 * 4 == bytes::dataset_record);
static_assert(3 * 4 + 2 * 2 + 4 * 8 + 32 == bytes::pack_record);
static_assert(8 + 2 * 4 + 8 + 2 * 4 + 2 * 2 + 3 * 4 + 2 + 2 + 4 + 16 + 8 == bytes::tile_index_record);
static_assert(4 + 2 * 2 + 4 * 4 + 3 * 8 + 16 == bytes::pack_header);
static_assert(2 * 2 + 4 * 1 + 2 * 2 + 7 * 4 == bytes::channel_record);
static_assert(5 * 2 + 2 * 1 + 4 == bytes::provenance_header);
static_assert(2 * 4 + 2 * 4 == bytes::provenance_palette_entry);

}  // namespace lunar::terrain::format_v1
