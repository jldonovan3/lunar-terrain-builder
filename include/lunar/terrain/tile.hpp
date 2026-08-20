#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <lunar/terrain/digest.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/ids.hpp>
#include <lunar/terrain/tile_key.hpp>

namespace lunar::terrain {

struct DatabaseHeader {
    std::uint16_t major_version{};
    std::uint16_t minor_version{};
    std::uint32_t flags{};
    DatabaseId database_id;
    double reference_radius_meters{};
    double elevation_origin_meters{};
    double elevation_step_meters{};
    std::uint16_t tile_cells{};
    std::uint16_t core_vertices{};
    std::uint8_t apron_samples{};
    std::uint8_t maximum_level{};
    ProjectionId projection{ProjectionId::lunar_qsc_v1};
    QuantizationId quantization{QuantizationId::global_u16_0p5m};
    std::uint64_t tile_count{};
    std::uint32_t dataset_count{};
    std::uint32_t pack_count{};
    Sha256Digest builder_configuration_hash;
    Sha256Digest dataset_registry_hash;
    Sha256Digest tile_index_hash;
    Sha256Digest database_content_hash;
};

struct TileIndexEntry {
    explicit TileIndexEntry(LunarTileKey tile_key) : key(tile_key) {}

    LunarTileKey key;
    PackId pack_id;
    std::uint32_t flags{};
    std::uint64_t payload_offset{};
    std::uint32_t stored_bytes{};
    std::uint32_t logical_channel_bytes{};
    std::uint16_t minimum_elevation_code{};
    std::uint16_t maximum_elevation_code{};
    DatasetId primary_dataset;
    std::uint32_t effective_resolution_millimeters{};
    std::uint32_t geometric_error_millimeters{};
    std::uint8_t materialized_child_mask{};
    std::uint8_t channel_count{};
    std::uint32_t payload_crc32c{};
    std::array<std::byte, 16> content_hash_prefix{};
    std::array<std::byte, 8> dependency_hash_prefix{};
};

struct ProvenancePaletteEntry {
    DatasetId dataset_id;
    std::uint32_t flags{};
    float contribution_fraction{};
    float native_resolution_meters{};
};

struct TileProvenance {
    std::uint16_t map_width{};
    std::uint16_t map_height{};
    std::vector<ProvenancePaletteEntry> palette;
    std::vector<std::uint16_t> dominant_source_indices;
};

class DecodedChannel {
public:
    DecodedChannel(
        ChannelId channel_id,
        std::uint16_t version,
        ElementType element_type,
        std::uint8_t components,
        std::uint16_t width,
        std::uint16_t height,
        std::uint32_t flags,
        std::uint32_t parameter1,
        std::vector<std::byte> bytes)
        : id_(channel_id),
          version_(version),
          element_type_(element_type),
          components_(components),
          width_(width),
          height_(height),
          flags_(flags),
          parameter1_(parameter1),
          bytes_(std::move(bytes)) {}

    [[nodiscard]] ChannelId id() const noexcept { return id_; }
    [[nodiscard]] std::uint16_t version() const noexcept { return version_; }
    [[nodiscard]] ElementType element_type() const noexcept { return element_type_; }
    [[nodiscard]] std::uint8_t components() const noexcept { return components_; }
    [[nodiscard]] std::uint16_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint16_t height() const noexcept { return height_; }
    [[nodiscard]] std::uint32_t flags() const noexcept { return flags_; }
    [[nodiscard]] std::uint32_t parameter1() const noexcept { return parameter1_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }

private:
    ChannelId id_;
    std::uint16_t version_;
    ElementType element_type_;
    std::uint8_t components_;
    std::uint16_t width_;
    std::uint16_t height_;
    std::uint32_t flags_;
    std::uint32_t parameter1_;
    std::vector<std::byte> bytes_;
};

struct DecodedTileMetadata {
    float effective_resolution_meters{};
    float geometric_error_meters{};
    float minimum_elevation_meters{};
    float maximum_elevation_meters{};
    DatasetId primary_dataset;
};

class DecodedTerrainTile {
public:
    DecodedTerrainTile(
        LunarTileKey key,
        DecodedTileMetadata metadata,
        std::vector<DecodedChannel> channels,
        std::optional<TileProvenance> provenance)
        : key_(key),
          metadata_(metadata),
          channels_(std::move(channels)),
          provenance_(std::move(provenance)) {}

    [[nodiscard]] LunarTileKey key() const noexcept { return key_; }
    [[nodiscard]] const DecodedTileMetadata& metadata() const noexcept { return metadata_; }
    [[nodiscard]] std::span<const DecodedChannel> channels() const noexcept { return channels_; }
    [[nodiscard]] const std::optional<TileProvenance>& provenance() const noexcept {
        return provenance_;
    }

private:
    LunarTileKey key_;
    DecodedTileMetadata metadata_;
    std::vector<DecodedChannel> channels_;
    std::optional<TileProvenance> provenance_;
};

}  // namespace lunar::terrain
