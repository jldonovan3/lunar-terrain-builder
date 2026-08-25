#include "builder/builder.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>

#include "builder/fusion.hpp"

namespace lunar::terrain::builder {
namespace {

[[nodiscard]] Error export_error(
    std::string message,
    const std::filesystem::path& path,
    const std::optional<LunarTileKey> key = std::nullopt) {
    Error error{ErrorCode::io_error, std::move(message)};
    error.with_path(path.string());
    if (key) {
        error.with_tile_key(key->encoded());
    }
    return error;
}

[[nodiscard]] const DecodedChannel* find_channel(
    const DecodedTerrainTile& tile,
    const ChannelId id) noexcept {
    const auto found = std::ranges::find_if(tile.channels(), [id](const DecodedChannel& channel) {
        return channel.id() == id;
    });
    return found == tile.channels().end() ? nullptr : &*found;
}

[[nodiscard]] Result<void> write_output(
    const std::filesystem::path& path,
    const std::span<const std::byte> bytes,
    const LunarTileKey key) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    if (!stream.is_open()) {
        return Result<void>::failure(export_error("could not create diagnostic output", path, key));
    }
    stream.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream) {
        return Result<void>::failure(export_error("could not write diagnostic output", path, key));
    }
    return Result<void>::success();
}

void append_text(std::vector<std::byte>& bytes, const std::string_view text) {
    const auto view = std::as_bytes(std::span{text});
    bytes.insert(bytes.end(), view.begin(), view.end());
}

[[nodiscard]] std::array<std::uint8_t, 3> dataset_color(const DatasetId id) noexcept {
    std::uint32_t value = id.value * 0x9E3779B9U;
    value ^= value >> 16U;
    return {
        static_cast<std::uint8_t>(48U + (value & 0xAFU)),
        static_cast<std::uint8_t>(48U + ((value >> 8U) & 0xAFU)),
        static_cast<std::uint8_t>(48U + ((value >> 16U) & 0xAFU)),
    };
}

[[nodiscard]] std::array<std::uint8_t, 3> quality_color(const std::uint8_t flags) noexcept {
    std::uint16_t red = 0;
    std::uint16_t green = 0;
    std::uint16_t blue = 0;
    if ((flags & quality_interpolated) != 0) {
        blue += 160;
    }
    if ((flags & quality_filled_no_data) != 0) {
        red += 180;
        blue += 180;
    }
    if ((flags & quality_fusion_transition) != 0) {
        red += 255;
        green += 128;
    }
    if ((flags & quality_bias_corrected) != 0) {
        green += 200;
        blue += 80;
    }
    if ((flags & quality_lower_confidence) != 0) {
        red += 120;
        green += 120;
    }
    return {
        static_cast<std::uint8_t>(std::min<std::uint16_t>(red, 255)),
        static_cast<std::uint8_t>(std::min<std::uint16_t>(green, 255)),
        static_cast<std::uint8_t>(std::min<std::uint16_t>(blue, 255)),
    };
}

[[nodiscard]] std::uint16_t read_u16(
    const std::span<const std::byte> bytes,
    const std::size_t index) noexcept {
    const std::size_t offset = index * 2U;
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] Result<std::vector<std::byte>> provenance_image(
    const DecodedTerrainTile& tile) {
    if (!tile.provenance() || tile.provenance()->palette.empty()) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::invalid_format, "tile has no provenance to export"});
    }
    const TileProvenance& provenance = *tile.provenance();
    if (!provenance.dominant_source_indices.empty() &&
        provenance.dominant_source_indices.size() != 64U * 64U) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::invalid_format, "tile provenance map is not 64x64"});
    }
    const DecodedChannel* quality = find_channel(tile, ChannelId::quality);
    std::vector<std::byte> bytes;
    append_text(bytes, "P6\n64 64\n255\n");
    bytes.reserve(bytes.size() + 64U * 64U * 3U);
    for (std::size_t index = 0; index < 64U * 64U; ++index) {
        const std::uint16_t palette_index = provenance.dominant_source_indices.empty()
            ? 0
            : provenance.dominant_source_indices[index];
        auto color = dataset_color(provenance.palette[palette_index].dataset_id);
        if (quality != nullptr &&
            (std::to_integer<std::uint8_t>(quality->bytes()[index]) &
             quality_fusion_transition) != 0) {
            color = {255, 128, 0};
        }
        for (const std::uint8_t component : color) {
            bytes.push_back(static_cast<std::byte>(component));
        }
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> quality_image(const DecodedTerrainTile& tile) {
    const DecodedChannel* quality = find_channel(tile, ChannelId::quality);
    if (quality == nullptr || quality->bytes().size() != 64U * 64U) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::not_found, "tile has no canonical 64x64 quality map"});
    }
    std::vector<std::byte> bytes;
    append_text(bytes, "P6\n64 64\n255\n");
    bytes.reserve(bytes.size() + 64U * 64U * 3U);
    for (const std::byte value : quality->bytes()) {
        const auto color = quality_color(std::to_integer<std::uint8_t>(value));
        for (const std::uint8_t component : color) {
            bytes.push_back(static_cast<std::byte>(component));
        }
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> transition_diagnostics(
    const DecodedTerrainTile& tile) {
    const DecodedChannel* elevation = find_channel(tile, ChannelId::elevation);
    const DecodedChannel* quality = find_channel(tile, ChannelId::quality);
    constexpr std::size_t elevation_samples =
        std::size_t{format_v1::serialized_elevation_samples} *
        format_v1::serialized_elevation_samples;
    if (elevation == nullptr || elevation->bytes().size() != elevation_samples * 2U ||
        quality == nullptr || quality->bytes().size() != 64U * 64U) {
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::not_found,
            "transition diagnostics require ELEV and canonical QUAL channels"});
    }
    std::vector<std::byte> bytes;
    append_text(
        bytes,
        "map_x,map_y,elevation_m,first_dx_m,first_dy_m,second_dx_m,second_dy_m,quality_flags\n");
    constexpr std::size_t width = format_v1::serialized_elevation_samples;
    for (std::size_t map_y = 0; map_y < 64; ++map_y) {
        for (std::size_t map_x = 0; map_x < 64; ++map_x) {
            const std::size_t quality_index = map_y * 64U + map_x;
            const std::uint8_t flags =
                std::to_integer<std::uint8_t>(quality->bytes()[quality_index]);
            if ((flags & quality_fusion_transition) == 0) {
                continue;
            }
            const std::size_t x = 1U + map_x * 4U + 2U;
            const std::size_t y = 1U + map_y * 4U + 2U;
            const auto meters = [&elevation](const std::size_t index) {
                return -16'384.0 + static_cast<double>(read_u16(elevation->bytes(), index)) * 0.5;
            };
            const double center = meters(y * width + x);
            const double west = meters(y * width + x - 1U);
            const double east = meters(y * width + x + 1U);
            const double south = meters((y - 1U) * width + x);
            const double north = meters((y + 1U) * width + x);
            append_text(bytes, fmt::format(
                "{},{},{:.17g},{:.17g},{:.17g},{:.17g},{:.17g},{}\n",
                map_x,
                map_y,
                center,
                (east - west) * 0.5,
                (north - south) * 0.5,
                east - 2.0 * center + west,
                north - 2.0 * center + south,
                flags));
        }
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

}  // namespace

Result<void> export_tile_diagnostic(
    const std::filesystem::path& database_path,
    const LunarTileKey key,
    const DiagnosticExportFormat format,
    const std::filesystem::path& output_path) {
    auto database = LunarTerrainDatabase::Open(database_path);
    if (!database) {
        return Result<void>::failure(std::move(database).error());
    }
    auto tile = database.value().ReadTile(key);
    if (!tile) {
        return Result<void>::failure(std::move(tile).error());
    }
    Result<std::vector<std::byte>> output = [&]() {
        switch (format) {
            case DiagnosticExportFormat::provenance_ppm:
                return provenance_image(tile.value());
            case DiagnosticExportFormat::quality_ppm:
                return quality_image(tile.value());
            case DiagnosticExportFormat::transition_csv:
                return transition_diagnostics(tile.value());
        }
        return Result<std::vector<std::byte>>::failure(Error{
            ErrorCode::invalid_argument, "unsupported diagnostic export format"});
    }();
    if (!output) {
        Error error = std::move(output).error();
        error.with_path(database_path.string()).with_tile_key(key.encoded());
        return Result<void>::failure(std::move(error));
    }
    return write_output(output_path, output.value(), key);
}

}  // namespace lunar::terrain::builder
