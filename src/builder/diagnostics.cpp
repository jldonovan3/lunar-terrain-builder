#include "builder/builder.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include <lunar/terrain/database.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/qsc_projection.hpp>

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

struct ExportVertex {
    double x_meters{};
    double y_meters{};
    double z_meters{};
    double latitude_radians{};
    double longitude_radians{};
    double elevation_meters{};
    std::uint16_t elevation_code{};
};

struct ExportTriangle {
    std::uint32_t first{};
    std::uint32_t second{};
    std::uint32_t third{};
};

[[nodiscard]] Result<std::vector<std::uint16_t>> core_elevation_codes(
    const DecodedTerrainTile& tile) {
    const DecodedChannel* elevation = find_channel(tile, ChannelId::elevation);
    constexpr std::size_t serialized_samples =
        std::size_t{format_v1::serialized_elevation_samples} *
        format_v1::serialized_elevation_samples;
    if (elevation == nullptr || elevation->bytes().size() != serialized_samples * 2U ||
        elevation->width() != format_v1::serialized_elevation_samples ||
        elevation->height() != format_v1::serialized_elevation_samples) {
        return Result<std::vector<std::uint16_t>>::failure(Error{
            ErrorCode::invalid_format,
            "canonical ELEV channel is required for this export"});
    }
    std::vector<std::uint16_t> codes;
    codes.reserve(
        std::size_t{format_v1::core_vertices} * format_v1::core_vertices);
    for (std::size_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::size_t x = 0; x < format_v1::core_vertices; ++x) {
            const std::size_t serialized_index =
                (y + 1U) * format_v1::serialized_elevation_samples + x + 1U;
            codes.push_back(read_u16(elevation->bytes(), serialized_index));
        }
    }
    return Result<std::vector<std::uint16_t>>::success(std::move(codes));
}

[[nodiscard]] Result<std::vector<ExportVertex>> reconstruct_vertices(
    const DecodedTerrainTile& tile,
    const DatabaseHeader& header) {
    auto codes = core_elevation_codes(tile);
    if (!codes) {
        return Result<std::vector<ExportVertex>>::failure(std::move(codes).error());
    }
    std::vector<ExportVertex> vertices;
    vertices.reserve(codes.value().size());
    const LunarTileKey key = tile.key();
    for (std::uint16_t y = 0; y < format_v1::core_vertices; ++y) {
        auto v = QscProjection::LatticeCoordinate(key.y(), y, key.level());
        if (!v) {
            return Result<std::vector<ExportVertex>>::failure(std::move(v).error());
        }
        for (std::uint16_t x = 0; x < format_v1::core_vertices; ++x) {
            auto u = QscProjection::LatticeCoordinate(key.x(), x, key.level());
            if (!u) {
                return Result<std::vector<ExportVertex>>::failure(std::move(u).error());
            }
            auto geographic = QscProjection::Inverse(QscCoordinate{
                static_cast<QscFace>(key.face()), u.value(), v.value(), 0.0});
            if (!geographic) {
                return Result<std::vector<ExportVertex>>::failure(std::move(geographic).error());
            }
            const std::size_t index = std::size_t{y} * format_v1::core_vertices + x;
            const std::uint16_t code = codes.value()[index];
            const double elevation =
                header.elevation_origin_meters +
                static_cast<double>(code) * header.elevation_step_meters;
            const double radius = header.reference_radius_meters + elevation;
            const double cos_latitude = std::cos(geographic.value().latitude_radians);
            vertices.push_back(ExportVertex{
                radius * cos_latitude * std::cos(geographic.value().longitude_radians),
                radius * cos_latitude * std::sin(geographic.value().longitude_radians),
                radius * std::sin(geographic.value().latitude_radians),
                geographic.value().latitude_radians,
                geographic.value().longitude_radians,
                elevation,
                code,
            });
        }
    }
    return Result<std::vector<ExportVertex>>::success(std::move(vertices));
}

[[nodiscard]] double outward_dot(
    const ExportVertex& first,
    const ExportVertex& second,
    const ExportVertex& third) noexcept {
    const double ab_x = second.x_meters - first.x_meters;
    const double ab_y = second.y_meters - first.y_meters;
    const double ab_z = second.z_meters - first.z_meters;
    const double ac_x = third.x_meters - first.x_meters;
    const double ac_y = third.y_meters - first.y_meters;
    const double ac_z = third.z_meters - first.z_meters;
    const double cross_x = ab_y * ac_z - ab_z * ac_y;
    const double cross_y = ab_z * ac_x - ab_x * ac_z;
    const double cross_z = ab_x * ac_y - ab_y * ac_x;
    return cross_x * first.x_meters + cross_y * first.y_meters +
           cross_z * first.z_meters;
}

[[nodiscard]] std::vector<ExportTriangle> mesh_triangles(
    const std::span<const ExportVertex> vertices) {
    std::vector<ExportTriangle> triangles;
    triangles.reserve(
        std::size_t{format_v1::tile_cells} * format_v1::tile_cells * 2U);
    const auto add_outward = [&vertices, &triangles](
                                 const std::uint32_t first,
                                 const std::uint32_t second,
                                 const std::uint32_t third) {
        if (outward_dot(vertices[first], vertices[second], vertices[third]) >= 0.0) {
            triangles.push_back(ExportTriangle{first, second, third});
        } else {
            triangles.push_back(ExportTriangle{first, third, second});
        }
    };
    for (std::uint32_t y = 0; y < format_v1::tile_cells; ++y) {
        for (std::uint32_t x = 0; x < format_v1::tile_cells; ++x) {
            const std::uint32_t southwest = y * format_v1::core_vertices + x;
            const std::uint32_t southeast = southwest + 1U;
            const std::uint32_t northwest = southwest + format_v1::core_vertices;
            const std::uint32_t northeast = northwest + 1U;
            add_outward(southwest, southeast, northeast);
            add_outward(southwest, northeast, northwest);
        }
    }
    return triangles;
}

[[nodiscard]] Result<std::vector<std::byte>> ply_mesh(
    const DecodedTerrainTile& tile,
    const DatabaseHeader& header) {
    auto vertices = reconstruct_vertices(tile, header);
    if (!vertices) {
        return Result<std::vector<std::byte>>::failure(std::move(vertices).error());
    }
    const std::vector<ExportTriangle> triangles = mesh_triangles(vertices.value());
    std::vector<std::byte> bytes;
    append_text(bytes, fmt::format(
        "ply\nformat ascii 1.0\ncomment lunar-terrain deterministic tile export\n"
        "element vertex {}\nproperty double x\nproperty double y\nproperty double z\n"
        "property ushort elevation_u16\nelement face {}\n"
        "property list uchar uint vertex_indices\nend_header\n",
        vertices.value().size(),
        triangles.size()));
    for (const ExportVertex& vertex : vertices.value()) {
        append_text(bytes, fmt::format(
            "{:.17g} {:.17g} {:.17g} {}\n",
            vertex.x_meters,
            vertex.y_meters,
            vertex.z_meters,
            vertex.elevation_code));
    }
    for (const ExportTriangle& triangle : triangles) {
        append_text(bytes, fmt::format(
            "3 {} {} {}\n", triangle.first, triangle.second, triangle.third));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> obj_mesh(
    const DecodedTerrainTile& tile,
    const DatabaseHeader& header) {
    auto vertices = reconstruct_vertices(tile, header);
    if (!vertices) {
        return Result<std::vector<std::byte>>::failure(std::move(vertices).error());
    }
    const std::vector<ExportTriangle> triangles = mesh_triangles(vertices.value());
    std::vector<std::byte> bytes;
    append_text(bytes, "# lunar-terrain deterministic Moon-centered tile export\n");
    for (const ExportVertex& vertex : vertices.value()) {
        append_text(bytes, fmt::format(
            "v {:.17g} {:.17g} {:.17g}\n",
            vertex.x_meters,
            vertex.y_meters,
            vertex.z_meters));
    }
    for (const ExportTriangle& triangle : triangles) {
        append_text(bytes, fmt::format(
            "f {} {} {}\n",
            triangle.first + 1U,
            triangle.second + 1U,
            triangle.third + 1U));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> elevation_pgm(
    const DecodedTerrainTile& tile) {
    auto codes = core_elevation_codes(tile);
    if (!codes) {
        return Result<std::vector<std::byte>>::failure(std::move(codes).error());
    }
    std::vector<std::byte> bytes;
    append_text(bytes, fmt::format(
        "P5\n{} {}\n65535\n", format_v1::core_vertices, format_v1::core_vertices));
    bytes.reserve(bytes.size() + codes.value().size() * 2U);
    for (const std::uint16_t code : codes.value()) {
        bytes.push_back(static_cast<std::byte>(code >> 8U));
        bytes.push_back(static_cast<std::byte>(code & 0xFFU));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> raw_elevation_u16_le(
    const DecodedTerrainTile& tile) {
    auto codes = core_elevation_codes(tile);
    if (!codes) {
        return Result<std::vector<std::byte>>::failure(std::move(codes).error());
    }
    std::vector<std::byte> bytes;
    bytes.reserve(codes.value().size() * 2U);
    for (const std::uint16_t code : codes.value()) {
        bytes.push_back(static_cast<std::byte>(code & 0xFFU));
        bytes.push_back(static_cast<std::byte>(code >> 8U));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<std::vector<std::byte>> sample_csv(
    const DecodedTerrainTile& tile,
    const DatabaseHeader& header) {
    auto vertices = reconstruct_vertices(tile, header);
    if (!vertices) {
        return Result<std::vector<std::byte>>::failure(std::move(vertices).error());
    }
    std::vector<std::byte> bytes;
    append_text(
        bytes,
        "sample_x,sample_y,latitude_degrees,longitude_degrees,elevation_m,elevation_u16,"
        "moon_x_m,moon_y_m,moon_z_m\n");
    constexpr double radians_to_degrees = 180.0 / std::numbers::pi_v<double>;
    for (std::uint32_t y = 0; y < format_v1::core_vertices; ++y) {
        for (std::uint32_t x = 0; x < format_v1::core_vertices; ++x) {
            const ExportVertex& vertex =
                vertices.value()[std::size_t{y} * format_v1::core_vertices + x];
            append_text(bytes, fmt::format(
                "{},{},{:.17g},{:.17g},{:.17g},{},{:.17g},{:.17g},{:.17g}\n",
                x,
                y,
                vertex.latitude_radians * radians_to_degrees,
                vertex.longitude_radians * radians_to_degrees,
                vertex.elevation_meters,
                vertex.elevation_code,
                vertex.x_meters,
                vertex.y_meters,
                vertex.z_meters));
        }
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
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

Result<void> export_tile(
    const std::filesystem::path& database_path,
    const LunarTileKey key,
    const DiagnosticExportFormat format,
    const std::filesystem::path& output_path,
    const ExecutionOptions& options) {
    if (options.telemetry != nullptr) {
        options.telemetry->SetPhase("export");
    }
    auto execution = check_execution(options);
    if (!execution) {
        return execution;
    }
    TelemetryActivity export_activity{options.telemetry, "export"};
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
            case DiagnosticExportFormat::ply:
                return ply_mesh(tile.value(), database.value().Header());
            case DiagnosticExportFormat::obj:
                return obj_mesh(tile.value(), database.value().Header());
            case DiagnosticExportFormat::elevation_pgm:
                return elevation_pgm(tile.value());
            case DiagnosticExportFormat::sample_csv:
                return sample_csv(tile.value(), database.value().Header());
            case DiagnosticExportFormat::raw_u16_le:
                return raw_elevation_u16_le(tile.value());
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

Result<void> export_tile_diagnostic(
    const std::filesystem::path& database_path,
    const LunarTileKey key,
    const DiagnosticExportFormat format,
    const std::filesystem::path& output_path,
    const ExecutionOptions& options) {
    return export_tile(database_path, key, format, output_path, options);
}

}  // namespace lunar::terrain::builder
