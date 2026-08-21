#include <lunar/terrain/database.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/evp.h>

#include <lunar/terrain/byte_io.hpp>
#include <lunar/terrain/codec.hpp>
#include <lunar/terrain/error.hpp>
#include <lunar/terrain/format_v1.hpp>
#include <lunar/terrain/integrity.hpp>

namespace lunar::terrain {
namespace {

using Bytes = std::span<const std::byte>;

constexpr std::uint64_t maximum_file_bytes = (std::uint64_t{1} << 63U) - 1U;
constexpr std::uint32_t chunk_required = 1U << 0U;
constexpr std::uint32_t chunk_content_identity = 1U << 1U;
constexpr std::uint32_t channel_required = 1U << 0U;

struct DigestContextDeleter {
    void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
};

class DigestAccumulator {
public:
    DigestAccumulator() : context_(EVP_MD_CTX_new()) {
        valid_ = context_ != nullptr && EVP_DigestInit_ex(context_.get(), EVP_sha256(), nullptr) == 1;
    }

    bool update(const Bytes bytes) noexcept {
        if (valid_ && EVP_DigestUpdate(context_.get(), bytes.data(), bytes.size()) != 1) {
            valid_ = false;
        }
        return valid_;
    }

    bool update_text(const std::string_view text) noexcept {
        return update(std::as_bytes(std::span{text}));
    }

    bool update_u8(const std::uint8_t value) noexcept {
        const std::array bytes{static_cast<std::byte>(value)};
        return update(bytes);
    }

    bool update_u16(const std::uint16_t value) noexcept {
        const std::array bytes{
            static_cast<std::byte>(value & 0xFFU),
            static_cast<std::byte>(value >> 8U),
        };
        return update(bytes);
    }

    bool update_u32(const std::uint32_t value) noexcept {
        std::array<std::byte, 4> bytes{};
        for (std::uint32_t index = 0; index < bytes.size(); ++index) {
            bytes[index] = static_cast<std::byte>(value >> (index * 8U));
        }
        return update(bytes);
    }

    bool update_u64(const std::uint64_t value) noexcept {
        std::array<std::byte, 8> bytes{};
        for (std::uint64_t index = 0; index < bytes.size(); ++index) {
            bytes[static_cast<std::size_t>(index)] =
                static_cast<std::byte>(value >> (index * 8U));
        }
        return update(bytes);
    }

    bool update_domain(const std::string_view domain) noexcept {
        return update_text(domain) && update_u8(0);
    }

    [[nodiscard]] Result<Sha256Digest> finish() noexcept {
        Sha256Digest digest;
        unsigned int byte_count = 0;
        if (!valid_ ||
            EVP_DigestFinal_ex(
                context_.get(),
                reinterpret_cast<unsigned char*>(digest.bytes.data()),
                &byte_count) != 1 ||
            byte_count != digest.bytes.size()) {
            return Result<Sha256Digest>::failure(
                Error{ErrorCode::internal_error, "OpenSSL SHA-256 calculation failed"});
        }
        valid_ = false;
        return Result<Sha256Digest>::success(digest);
    }

private:
    std::unique_ptr<EVP_MD_CTX, DigestContextDeleter> context_;
    bool valid_{};
};

struct ChunkInfo {
    std::array<std::byte, 4> tag{};
    std::uint16_t version{};
    std::uint16_t flags{};
    std::uint64_t offset{};
    std::uint64_t stored_bytes{};
    std::uint64_t logical_bytes{};
    Bytes bytes;
};

struct PackInfo {
    PackId id;
    std::filesystem::path relative_path;
    std::uint16_t default_codec{};
    std::uint64_t tile_count{};
    std::uint64_t file_bytes{};
    LunarTileKey first_key;
    LunarTileKey last_key;
    Sha256Digest hash;
};

struct DatabaseState {
    std::filesystem::path manifest_path;
    DatabaseHeader header;
    std::vector<TileIndexEntry> index;
    std::vector<PackInfo> packs;
    std::set<std::uint32_t> dataset_ids;
};

struct HeaderParse {
    DatabaseHeader header;
    std::uint32_t chunk_count{};
    std::uint64_t chunk_directory_offset{};
};

struct ChannelRecord {
    std::uint16_t id{};
    std::uint16_t version{};
    std::uint8_t element_type{};
    std::uint8_t components{};
    std::uint8_t codec{};
    std::uint8_t predictor{};
    std::uint16_t width{};
    std::uint16_t height{};
    std::uint32_t flags{};
    std::uint32_t data_offset{};
    std::uint32_t stored_bytes{};
    std::uint32_t logical_bytes{};
    std::uint32_t crc{};
    std::uint32_t parameter0{};
    std::uint32_t parameter1{};
    std::vector<std::byte> decoded;
};

[[nodiscard]] Error make_error(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path,
    const std::optional<std::uint64_t> offset = std::nullopt) {
    Error error{code, std::move(message)};
    error.with_path(path.string());
    if (offset) {
        error.with_offset(*offset);
    }
    return error;
}

template <typename T>
[[nodiscard]] Result<T> failure(
    const ErrorCode code,
    std::string message,
    const std::filesystem::path& path,
    const std::optional<std::uint64_t> offset = std::nullopt) {
    return Result<T>::failure(make_error(code, std::move(message), path, offset));
}

[[nodiscard]] std::uint8_t read_u8(const Bytes bytes, const std::size_t offset) noexcept {
    return std::to_integer<std::uint8_t>(bytes[offset]);
}

[[nodiscard]] std::uint16_t read_u16(const Bytes bytes, const std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint16_t>(bytes[offset]) |
        (std::to_integer<std::uint16_t>(bytes[offset + 1U]) << 8U));
}

[[nodiscard]] std::uint32_t read_u32(const Bytes bytes, const std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (std::uint32_t index = 0; index < 4; ++index) {
        value |= std::to_integer<std::uint32_t>(bytes[offset + index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const Bytes bytes, const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::uint64_t index = 0; index < 8; ++index) {
        value |= std::to_integer<std::uint64_t>(bytes[offset + static_cast<std::size_t>(index)]) <<
                 (index * 8U);
    }
    return value;
}

[[nodiscard]] float read_f32(const Bytes bytes, const std::size_t offset) noexcept {
    return std::bit_cast<float>(read_u32(bytes, offset));
}

[[nodiscard]] double read_f64(const Bytes bytes, const std::size_t offset) noexcept {
    return std::bit_cast<double>(read_u64(bytes, offset));
}

[[nodiscard]] bool range_fits(
    const std::uint64_t offset,
    const std::uint64_t byte_count,
    const std::uint64_t containing_bytes) noexcept {
    return offset <= containing_bytes && byte_count <= containing_bytes - offset;
}

[[nodiscard]] std::uint64_t align8(const std::uint64_t value) noexcept {
    return (value + 7U) & ~std::uint64_t{7};
}

[[nodiscard]] bool all_zero(const Bytes bytes) noexcept {
    return std::ranges::all_of(bytes, [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] bool tag_is(
    const std::array<std::byte, 4>& tag,
    const std::string_view expected) noexcept {
    return expected.size() == tag.size() &&
           std::equal(tag.begin(), tag.end(), expected.begin(), [](const std::byte left, const char right) {
               return left == static_cast<std::byte>(static_cast<unsigned char>(right));
           });
}

[[nodiscard]] bool magic_is(
    const Bytes bytes,
    const std::size_t offset,
    const std::string_view expected) noexcept {
    return range_fits(offset, expected.size(), bytes.size()) &&
           std::equal(expected.begin(), expected.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                      [](const char left, const std::byte right) {
                          return static_cast<std::byte>(static_cast<unsigned char>(left)) == right;
                      });
}

[[nodiscard]] Sha256Digest read_digest(const Bytes bytes, const std::size_t offset) {
    Sha256Digest digest;
    std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), digest.bytes.size(), digest.bytes.begin());
    return digest;
}

[[nodiscard]] bool digest_prefix_matches(
    const Sha256Digest& digest,
    const Bytes prefix) noexcept {
    return prefix.size() <= digest.bytes.size() &&
           std::equal(prefix.begin(), prefix.end(), digest.bytes.begin());
}

[[nodiscard]] bool unsigned_utf8_less(const std::string& left, const std::string& right) noexcept {
    return std::lexicographical_compare(
        left.begin(), left.end(), right.begin(), right.end(), [](const char a, const char b) {
            return static_cast<unsigned char>(a) < static_cast<unsigned char>(b);
        });
}

[[nodiscard]] bool valid_utf8(const Bytes text, const bool reject_nul) noexcept {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto first = std::to_integer<std::uint8_t>(text[index]);
        if (first <= 0x7FU) {
            if (reject_nul && first == 0) {
                return false;
            }
            ++index;
            continue;
        }

        std::size_t continuation_count = 0;
        std::uint8_t second_min = 0x80U;
        std::uint8_t second_max = 0xBFU;
        if (first >= 0xC2U && first <= 0xDFU) {
            continuation_count = 1;
        } else if (first >= 0xE0U && first <= 0xEFU) {
            continuation_count = 2;
            if (first == 0xE0U) {
                second_min = 0xA0U;
            } else if (first == 0xEDU) {
                second_max = 0x9FU;
            }
        } else if (first >= 0xF0U && first <= 0xF4U) {
            continuation_count = 3;
            if (first == 0xF0U) {
                second_min = 0x90U;
            } else if (first == 0xF4U) {
                second_max = 0x8FU;
            }
        } else {
            return false;
        }

        if (continuation_count > text.size() - index - 1U) {
            return false;
        }
        const auto second = std::to_integer<std::uint8_t>(text[index + 1U]);
        if (second < second_min || second > second_max) {
            return false;
        }
        for (std::size_t continuation = 2; continuation <= continuation_count; ++continuation) {
            const auto value = std::to_integer<std::uint8_t>(text[index + continuation]);
            if (value < 0x80U || value > 0xBFU) {
                return false;
            }
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] Result<std::vector<std::byte>> read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream.is_open()) {
        return failure<std::vector<std::byte>>(
            ErrorCode::io_error, "could not open file", path);
    }
    const std::streamoff end = stream.tellg();
    if (end < 0 || static_cast<std::uint64_t>(end) > maximum_file_bytes ||
        static_cast<std::uint64_t>(end) > std::numeric_limits<std::size_t>::max() ||
        static_cast<std::uint64_t>(end) >
            static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
        return failure<std::vector<std::byte>>(
            ErrorCode::arithmetic_overflow, "file size is not representable", path);
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(end));
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty()) {
        stream.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(end));
    }
    if (!stream) {
        return failure<std::vector<std::byte>>(
            ErrorCode::io_error, "could not read complete file", path);
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] Result<HeaderParse> parse_header(
    const Bytes bytes,
    const std::filesystem::path& path) {
    using namespace format_v1;
    if (bytes.size() < format_v1::bytes::ltdb_header) {
        return failure<HeaderParse>(
            ErrorCode::truncated_data, "LTDB header is truncated", path, bytes.size());
    }
    const Bytes record = bytes.first(format_v1::bytes::ltdb_header);
    if (!magic_is(record, ltdb_header_offset::magic, "LTDB")) {
        return failure<HeaderParse>(ErrorCode::invalid_format, "LTDB magic is invalid", path, 0);
    }

    const std::uint16_t major = read_u16(record, ltdb_header_offset::major);
    const std::uint16_t minor = read_u16(record, ltdb_header_offset::minor);
    if (major != major_version) {
        return failure<HeaderParse>(
            ErrorCode::unsupported_version, "LTDB major version is not supported", path, 4);
    }
    if (read_u32(record, ltdb_header_offset::header_bytes) != format_v1::bytes::ltdb_header ||
        read_u32(record, ltdb_header_offset::endian) != endian_tag) {
        return failure<HeaderParse>(
            ErrorCode::invalid_format, "LTDB fixed header size or endian tag is invalid", path, 8);
    }
    if (read_u32(record, ltdb_header_offset::flags) != 0) {
        return failure<HeaderParse>(
            ErrorCode::unsupported_feature, "LTDB contains unknown database flags", path, 16);
    }
    if (!all_zero(record.subspan(ltdb_header_offset::reserved, 32))) {
        return failure<HeaderParse>(
            ErrorCode::invalid_format, "LTDB reserved header bytes are nonzero", path,
            ltdb_header_offset::reserved);
    }

    const std::uint32_t chunk_count = read_u32(record, ltdb_header_offset::chunk_count);
    const std::uint64_t directory_offset = read_u64(record, ltdb_header_offset::chunk_directory);
    if (chunk_count < 5U || chunk_count > 4096U || directory_offset != format_v1::bytes::ltdb_header) {
        return failure<HeaderParse>(
            ErrorCode::invalid_format, "LTDB chunk directory count or offset is invalid", path, 20);
    }

    DatabaseHeader header;
    header.major_version = major;
    header.minor_version = minor;
    header.flags = 0;
    std::copy_n(
        record.begin() + static_cast<std::ptrdiff_t>(ltdb_header_offset::database_id),
        header.database_id.bytes.size(),
        header.database_id.bytes.begin());
    header.reference_radius_meters = read_f64(record, ltdb_header_offset::reference_radius);
    header.elevation_origin_meters = read_f64(record, ltdb_header_offset::elevation_origin);
    header.elevation_step_meters = read_f64(record, ltdb_header_offset::elevation_step);
    header.tile_cells = read_u16(record, ltdb_header_offset::tile_cells);
    header.core_vertices = read_u16(record, ltdb_header_offset::core_vertices);
    header.apron_samples = read_u8(record, ltdb_header_offset::apron);
    header.maximum_level = read_u8(record, ltdb_header_offset::maximum_level);
    header.projection = static_cast<ProjectionId>(read_u8(record, ltdb_header_offset::projection));
    header.quantization = static_cast<QuantizationId>(read_u8(record, ltdb_header_offset::quantization));
    header.tile_count = read_u64(record, ltdb_header_offset::tile_count);
    header.dataset_count = read_u32(record, ltdb_header_offset::dataset_count);
    header.pack_count = read_u32(record, ltdb_header_offset::pack_count);
    header.builder_configuration_hash =
        read_digest(record, ltdb_header_offset::builder_configuration_hash);
    header.dataset_registry_hash = read_digest(record, ltdb_header_offset::dataset_registry_hash);
    header.tile_index_hash = read_digest(record, ltdb_header_offset::tile_index_hash);
    header.database_content_hash = read_digest(record, ltdb_header_offset::database_content_hash);

    if (!std::isfinite(header.reference_radius_meters) || header.reference_radius_meters <= 0.0 ||
        !std::isfinite(header.elevation_origin_meters) ||
        !std::isfinite(header.elevation_step_meters) || header.elevation_step_meters <= 0.0) {
        return failure<HeaderParse>(
            ErrorCode::invalid_format, "LTDB quantization or radius values are invalid", path, 48);
    }
    if (header.tile_cells != tile_cells || header.core_vertices != core_vertices ||
        header.apron_samples != apron_samples || header.maximum_level > LunarTileKey::max_level) {
        return failure<HeaderParse>(
            ErrorCode::invalid_format, "LTDB tile-shape fields are invalid", path, 72);
    }
    if (header.projection != ProjectionId::lunar_qsc_v1 ||
        header.quantization != QuantizationId::global_u16_0p5m) {
        return failure<HeaderParse>(
            ErrorCode::unsupported_feature, "LTDB projection or quantization is not supported", path, 78);
    }

    return Result<HeaderParse>::success(HeaderParse{header, chunk_count, directory_offset});
}

[[nodiscard]] Result<std::vector<ChunkInfo>> parse_chunks(
    const Bytes file,
    const HeaderParse& parsed_header,
    const std::filesystem::path& path) {
    using namespace format_v1;
    const std::uint64_t directory_bytes =
        std::uint64_t{parsed_header.chunk_count} * format_v1::bytes::chunk_directory_entry;
    if (!range_fits(parsed_header.chunk_directory_offset, directory_bytes, file.size())) {
        return failure<std::vector<ChunkInfo>>(
            ErrorCode::truncated_data, "LTDB chunk directory is truncated", path,
            parsed_header.chunk_directory_offset);
    }

    std::vector<ChunkInfo> chunks;
    chunks.reserve(parsed_header.chunk_count);
    std::optional<std::array<std::byte, 4>> previous_tag;
    for (std::uint32_t index = 0; index < parsed_header.chunk_count; ++index) {
        const std::uint64_t record_offset =
            parsed_header.chunk_directory_offset +
            std::uint64_t{index} * format_v1::bytes::chunk_directory_entry;
        const Bytes record = file.subspan(
            static_cast<std::size_t>(record_offset), format_v1::bytes::chunk_directory_entry);

        ChunkInfo chunk;
        std::copy_n(
            record.begin() + static_cast<std::ptrdiff_t>(chunk_directory_offset::tag),
            chunk.tag.size(),
            chunk.tag.begin());
        chunk.version = read_u16(record, chunk_directory_offset::version);
        chunk.flags = read_u16(record, chunk_directory_offset::flags);
        chunk.offset = read_u64(record, chunk_directory_offset::file_offset);
        chunk.stored_bytes = read_u64(record, chunk_directory_offset::stored_bytes);
        chunk.logical_bytes = read_u64(record, chunk_directory_offset::logical_bytes);
        const std::uint32_t expected_crc = read_u32(record, chunk_directory_offset::crc32c);

        if (previous_tag && !std::lexicographical_compare(
                                previous_tag->begin(), previous_tag->end(),
                                chunk.tag.begin(), chunk.tag.end())) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "LTDB chunk directory tags are not strictly sorted", path,
                record_offset);
        }
        previous_tag = chunk.tag;
        if ((chunk.flags & ~std::uint16_t{0x0003}) != 0) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::unsupported_feature, "LTDB chunk has unknown flags", path, record_offset + 6U);
        }
        if (read_u32(record, chunk_directory_offset::reserved) != 0) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "LTDB chunk directory reserved bytes are nonzero", path,
                record_offset + chunk_directory_offset::reserved);
        }
        if (chunk.offset % 8U != 0 || !range_fits(chunk.offset, chunk.stored_bytes, file.size())) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "LTDB chunk range is invalid or misaligned", path,
                record_offset + chunk_directory_offset::file_offset);
        }
        chunk.bytes = file.subspan(
            static_cast<std::size_t>(chunk.offset), static_cast<std::size_t>(chunk.stored_bytes));
        if (crc32c(chunk.bytes) != expected_crc) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::checksum_mismatch, "LTDB chunk CRC32C does not match", path, chunk.offset);
        }

        const bool known = tag_is(chunk.tag, "DSET") || tag_is(chunk.tag, "META") ||
                           tag_is(chunk.tag, "PACK") || tag_is(chunk.tag, "STRS") ||
                           tag_is(chunk.tag, "TIDX");
        if (known) {
            if (chunk.version != 1) {
                return failure<std::vector<ChunkInfo>>(
                    ErrorCode::unsupported_version, "known LTDB chunk version is not supported", path,
                    record_offset + 4U);
            }
            if (chunk.flags != 0x0003U || chunk.stored_bytes != chunk.logical_bytes) {
                return failure<std::vector<ChunkInfo>>(
                    ErrorCode::invalid_format, "known LTDB chunk framing is not canonical v1", path,
                    record_offset + 6U);
            }
        } else if ((chunk.flags & chunk_required) != 0) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::unsupported_feature, "unknown required LTDB chunk is not supported", path,
                record_offset);
        }
        chunks.push_back(chunk);
    }

    const std::uint64_t directory_end = parsed_header.chunk_directory_offset + directory_bytes;
    std::vector<const ChunkInfo*> physical;
    physical.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        physical.push_back(&chunk);
    }
    std::ranges::sort(physical, {}, &ChunkInfo::offset);
    std::uint64_t previous_end = directory_end;
    for (const ChunkInfo* chunk : physical) {
        if (chunk->offset < previous_end) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "LTDB chunks overlap the directory or one another", path,
                chunk->offset);
        }
        if (!all_zero(file.subspan(
                static_cast<std::size_t>(previous_end),
                static_cast<std::size_t>(chunk->offset - previous_end)))) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "LTDB alignment bytes are nonzero", path, previous_end);
        }
        previous_end = chunk->offset + chunk->stored_bytes;
    }

    for (const std::string_view required : {"DSET", "META", "PACK", "STRS", "TIDX"}) {
        const auto count = std::ranges::count_if(chunks, [required](const ChunkInfo& chunk) {
            return tag_is(chunk.tag, required);
        });
        if (count != 1) {
            return failure<std::vector<ChunkInfo>>(
                ErrorCode::invalid_format, "mandatory LTDB chunk is missing or duplicated", path);
        }
    }
    return Result<std::vector<ChunkInfo>>::success(std::move(chunks));
}

[[nodiscard]] const ChunkInfo& find_chunk(
    const std::vector<ChunkInfo>& chunks,
    const std::string_view tag) {
    return *std::ranges::find_if(chunks, [tag](const ChunkInfo& chunk) {
        return tag_is(chunk.tag, tag);
    });
}

[[nodiscard]] Result<std::map<std::uint32_t, std::string>> parse_strings(
    const ChunkInfo& chunk,
    const std::filesystem::path& path) {
    std::map<std::uint32_t, std::string> strings;
    std::uint64_t offset = 0;
    std::string previous;
    while (offset < chunk.bytes.size()) {
        if (offset % 4U != 0 || !range_fits(offset, 4, chunk.bytes.size())) {
            return failure<std::map<std::uint32_t, std::string>>(
                ErrorCode::invalid_format, "STRS record header is invalid", path, chunk.offset + offset);
        }
        const std::uint32_t byte_count = read_u32(chunk.bytes, static_cast<std::size_t>(offset));
        if (!range_fits(offset + 4U, byte_count, chunk.bytes.size())) {
            return failure<std::map<std::uint32_t, std::string>>(
                ErrorCode::truncated_data, "STRS record text is truncated", path, chunk.offset + offset);
        }
        const Bytes text = chunk.bytes.subspan(
            static_cast<std::size_t>(offset + 4U), byte_count);
        if (!valid_utf8(text, true)) {
            return failure<std::map<std::uint32_t, std::string>>(
                ErrorCode::invalid_format, "STRS contains invalid UTF-8 or NUL", path, chunk.offset + offset + 4U);
        }
        const std::string value(reinterpret_cast<const char*>(text.data()), text.size());
        if (offset == 0) {
            if (!value.empty()) {
                return failure<std::map<std::uint32_t, std::string>>(
                    ErrorCode::invalid_format, "STRS record zero is not empty", path, chunk.offset);
            }
        } else if (value.empty() || !unsigned_utf8_less(previous, value)) {
            return failure<std::map<std::uint32_t, std::string>>(
                ErrorCode::invalid_format, "STRS strings are empty, duplicated, or unsorted", path,
                chunk.offset + offset);
        }
        strings.emplace(static_cast<std::uint32_t>(offset), value);
        previous = value;

        const std::uint64_t unaligned_end = offset + 4U + byte_count;
        const std::uint64_t record_end = (unaligned_end + 3U) & ~std::uint64_t{3};
        if (!range_fits(unaligned_end, record_end - unaligned_end, chunk.bytes.size()) ||
            !all_zero(chunk.bytes.subspan(
                static_cast<std::size_t>(unaligned_end),
                static_cast<std::size_t>(record_end - unaligned_end)))) {
            return failure<std::map<std::uint32_t, std::string>>(
                ErrorCode::invalid_format, "STRS padding is invalid", path, chunk.offset + unaligned_end);
        }
        offset = record_end;
    }
    if (strings.empty()) {
        return failure<std::map<std::uint32_t, std::string>>(
            ErrorCode::invalid_format, "STRS does not contain record zero", path, chunk.offset);
    }
    return Result<std::map<std::uint32_t, std::string>>::success(std::move(strings));
}

[[nodiscard]] bool canonical_nan(const std::uint64_t bits) noexcept {
    return bits == 0x7FF8000000000000ULL;
}

[[nodiscard]] Result<Sha256Digest> validate_datasets(
    const ChunkInfo& dset,
    const ChunkInfo& meta,
    const std::map<std::uint32_t, std::string>& strings,
    const DatabaseHeader& header,
    const std::filesystem::path& path,
    std::set<std::uint32_t>& dataset_ids) {
    using namespace format_v1;
    if (dset.bytes.size() % format_v1::bytes::dataset_record != 0 ||
        dset.bytes.size() / format_v1::bytes::dataset_record != header.dataset_count) {
        return failure<Sha256Digest>(
            ErrorCode::invalid_format, "DSET byte count disagrees with the header", path, dset.offset);
    }
    if (!valid_utf8(meta.bytes, true)) {
        return failure<Sha256Digest>(
            ErrorCode::invalid_format, "META contains invalid UTF-8 or NUL", path, meta.offset);
    }

    DigestAccumulator hash;
    hash.update_domain("LTDB_DATASET_REGISTRY_V1");
    hash.update_u32(header.dataset_count);
    std::uint32_t previous_id = 0;
    bool first = true;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> meta_ranges;
    for (std::uint32_t index = 0; index < header.dataset_count; ++index) {
        const std::uint64_t record_offset = std::uint64_t{index} * format_v1::bytes::dataset_record;
        const Bytes record = dset.bytes.subspan(
            static_cast<std::size_t>(record_offset), format_v1::bytes::dataset_record);
        const std::uint32_t id = read_u32(record, dataset_record_offset::dataset_id);
        const std::uint32_t flags = read_u32(record, dataset_record_offset::flags);
        if ((!first && id <= previous_id) || (flags & ~0x00000003U) != 0 ||
            read_u32(record, dataset_record_offset::reserved) != 0) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET IDs, flags, or reserved bytes are invalid", path,
                dset.offset + record_offset);
        }
        first = false;
        previous_id = id;
        dataset_ids.insert(id);

        hash.update_u32(id);
        hash.update_u32(flags);
        for (std::size_t field = 0; field < 8; ++field) {
            const std::uint32_t string_id =
                read_u32(record, dataset_record_offset::first_string_id + field * 4U);
            const auto found = strings.find(string_id);
            if (found == strings.end()) {
                return failure<Sha256Digest>(
                    ErrorCode::invalid_format, "DSET StringID does not point to a STRS record", path,
                    dset.offset + record_offset + dataset_record_offset::first_string_id + field * 4U);
            }
            hash.update_u64(found->second.size());
            hash.update_text(found->second);
        }

        const double nominal = read_f64(record, dataset_record_offset::nominal_resolution);
        if (!std::isfinite(nominal) || nominal <= 0.0) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET nominal resolution is invalid", path,
                dset.offset + record_offset + dataset_record_offset::nominal_resolution);
        }
        for (const std::size_t field : {
                 dataset_record_offset::horizontal_accuracy,
                 dataset_record_offset::vertical_accuracy}) {
            const double value = read_f64(record, field);
            const std::uint64_t bits = read_u64(record, field);
            if ((!std::isfinite(value) || value <= 0.0) && !canonical_nan(bits)) {
                return failure<Sha256Digest>(
                    ErrorCode::invalid_format, "DSET accuracy value is not positive or canonical NaN", path,
                    dset.offset + record_offset + field);
            }
        }
        const std::uint64_t no_data_bits = read_u64(record, dataset_record_offset::source_no_data);
        const double no_data = std::bit_cast<double>(no_data_bits);
        if ((flags & 0x2U) == 0) {
            if (!canonical_nan(no_data_bits)) {
                return failure<Sha256Digest>(
                    ErrorCode::invalid_format, "DSET undeclared no-data value is not canonical NaN", path,
                    dset.offset + record_offset + dataset_record_offset::source_no_data);
            }
        } else if (!std::isfinite(no_data) && !canonical_nan(no_data_bits)) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET no-data value is invalid", path,
                dset.offset + record_offset + dataset_record_offset::source_no_data);
        }
        hash.update(record.subspan(dataset_record_offset::nominal_resolution, 32));

        hash.update_u64(read_u64(record, dataset_record_offset::artifact_bundle_bytes));
        hash.update(record.subspan(dataset_record_offset::artifact_bundle_hash, 32));

        const std::uint32_t metadata_offset = read_u32(record, dataset_record_offset::meta_offset);
        const std::uint32_t metadata_bytes = read_u32(record, dataset_record_offset::meta_bytes);
        if (metadata_bytes == 0) {
            if (metadata_offset != 0) {
                return failure<Sha256Digest>(
                    ErrorCode::invalid_format, "empty DSET META reference has a nonzero offset", path,
                    dset.offset + record_offset + dataset_record_offset::meta_offset);
            }
        } else if (!range_fits(metadata_offset, metadata_bytes, meta.bytes.size())) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET META range is invalid", path,
                dset.offset + record_offset + dataset_record_offset::meta_offset);
        } else {
            meta_ranges.emplace_back(metadata_offset, std::uint64_t{metadata_offset} + metadata_bytes);
        }
        const Bytes metadata = meta.bytes.subspan(metadata_offset, metadata_bytes);
        if (!metadata.empty() &&
            (metadata.front() != std::byte{'{'} || metadata.back() != std::byte{'}'})) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET META document is not a JSON object", path,
                meta.offset + metadata_offset);
        }
        hash.update_u64(metadata_bytes);
        hash.update(metadata);
        hash.update_u32(read_u32(record, dataset_record_offset::quality_schema));
    }

    std::ranges::sort(meta_ranges);
    for (std::size_t index = 1; index < meta_ranges.size(); ++index) {
        const auto& previous = meta_ranges[index - 1U];
        const auto& current = meta_ranges[index];
        if (current.first < previous.second && current != previous) {
            return failure<Sha256Digest>(
                ErrorCode::invalid_format, "DSET META ranges partially overlap", path,
                meta.offset + current.first);
        }
    }
    return hash.finish();
}

[[nodiscard]] bool portable_relative_path(const std::string& value) noexcept {
    if (value.empty() || value.front() == '/' || value.find('\\') != std::string::npos ||
        (value.size() >= 2 && ((value[0] >= 'A' && value[0] <= 'Z') ||
                             (value[0] >= 'a' && value[0] <= 'z')) &&
         value[1] == ':')) {
        return false;
    }
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const std::size_t slash = value.find('/', begin);
        const std::string_view segment = slash == std::string::npos
                                             ? std::string_view{value}.substr(begin)
                                             : std::string_view{value}.substr(begin, slash - begin);
        if (segment.empty() || segment == "." || segment == "..") {
            return false;
        }
        if (slash == std::string::npos) {
            break;
        }
        begin = slash + 1U;
    }
    return true;
}

[[nodiscard]] Result<std::vector<PackInfo>> parse_packs(
    const ChunkInfo& pack_chunk,
    const std::map<std::uint32_t, std::string>& strings,
    const DatabaseHeader& header,
    const std::filesystem::path& path) {
    using namespace format_v1;
    if (pack_chunk.bytes.size() % format_v1::bytes::pack_record != 0 ||
        pack_chunk.bytes.size() / format_v1::bytes::pack_record != header.pack_count) {
        return failure<std::vector<PackInfo>>(
            ErrorCode::invalid_format, "PACK byte count disagrees with the header", path,
            pack_chunk.offset);
    }

    std::vector<PackInfo> packs;
    packs.reserve(header.pack_count);
    std::uint32_t previous_id = 0;
    bool first = true;
    for (std::uint32_t index = 0; index < header.pack_count; ++index) {
        const std::uint64_t record_offset = std::uint64_t{index} * format_v1::bytes::pack_record;
        const Bytes record = pack_chunk.bytes.subspan(
            static_cast<std::size_t>(record_offset), format_v1::bytes::pack_record);
        const std::uint32_t id = read_u32(record, pack_record_offset::pack_id);
        const std::uint32_t flags = read_u32(record, pack_record_offset::flags);
        if ((!first && id <= previous_id) || flags != 0 ||
            read_u16(record, pack_record_offset::reserved) != 0) {
            return failure<std::vector<PackInfo>>(
                ErrorCode::invalid_format, "PACK IDs, flags, or reserved bytes are invalid", path,
                pack_chunk.offset + record_offset);
        }
        first = false;
        previous_id = id;

        const std::uint32_t path_id = read_u32(record, pack_record_offset::path_string_id);
        const auto found_path = strings.find(path_id);
        if (found_path == strings.end() || !portable_relative_path(found_path->second)) {
            return failure<std::vector<PackInfo>>(
                ErrorCode::invalid_format, "PACK path is not a portable relative STRS path", path,
                pack_chunk.offset + record_offset + pack_record_offset::path_string_id);
        }
        const std::uint16_t default_codec = read_u16(record, pack_record_offset::default_codec);
        if (default_codec > static_cast<std::uint16_t>(Codec::zstandard)) {
            return failure<std::vector<PackInfo>>(
                ErrorCode::unsupported_feature, "PACK default codec is not supported", path,
                pack_chunk.offset + record_offset + pack_record_offset::default_codec);
        }
        const std::uint64_t tile_count = read_u64(record, pack_record_offset::tile_count);
        const std::uint64_t file_bytes = read_u64(record, pack_record_offset::file_bytes);
        auto first_key = LunarTileKey::from_encoded(read_u64(record, pack_record_offset::first_tile_key));
        auto last_key = LunarTileKey::from_encoded(read_u64(record, pack_record_offset::last_tile_key));
        if (tile_count == 0 || file_bytes < format_v1::bytes::pack_header || !first_key || !last_key ||
            first_key.value() > last_key.value() || first_key.value().face() != last_key.value().face() ||
            first_key.value().level() != last_key.value().level()) {
            return failure<std::vector<PackInfo>>(
                ErrorCode::invalid_format, "PACK range or tile-key bounds are invalid", path,
                pack_chunk.offset + record_offset + pack_record_offset::tile_count);
        }

        packs.push_back(PackInfo{
            PackId{id},
            std::filesystem::path{found_path->second},
            default_codec,
            tile_count,
            file_bytes,
            first_key.value(),
            last_key.value(),
            read_digest(record, pack_record_offset::sha256),
        });
    }
    return Result<std::vector<PackInfo>>::success(std::move(packs));
}

[[nodiscard]] const PackInfo* find_pack(
    const std::vector<PackInfo>& packs,
    const PackId id) noexcept {
    const auto found = std::ranges::lower_bound(packs, id.value, {}, [](const PackInfo& pack) {
        return pack.id.value;
    });
    return found != packs.end() && found->id == id ? &*found : nullptr;
}

[[nodiscard]] Result<std::vector<TileIndexEntry>> parse_tile_index(
    const ChunkInfo& tidx,
    const DatabaseHeader& header,
    const std::vector<PackInfo>& packs,
    const std::set<std::uint32_t>& datasets,
    const std::filesystem::path& path) {
    using namespace format_v1;
    if (tidx.bytes.size() % format_v1::bytes::tile_index_record != 0 ||
        tidx.bytes.size() / format_v1::bytes::tile_index_record != header.tile_count ||
        header.tile_count > std::numeric_limits<std::size_t>::max()) {
        return failure<std::vector<TileIndexEntry>>(
            ErrorCode::invalid_format, "TIDX byte count disagrees with the header", path, tidx.offset);
    }

    std::vector<TileIndexEntry> entries;
    entries.reserve(static_cast<std::size_t>(header.tile_count));
    for (std::uint64_t index = 0; index < header.tile_count; ++index) {
        const std::uint64_t record_offset = index * format_v1::bytes::tile_index_record;
        const Bytes record = tidx.bytes.subspan(
            static_cast<std::size_t>(record_offset), format_v1::bytes::tile_index_record);
        auto key = LunarTileKey::from_encoded(read_u64(record, tile_index_offset::tile_key));
        if (!key || (!entries.empty() && key.value() <= entries.back().key)) {
            return failure<std::vector<TileIndexEntry>>(
                ErrorCode::invalid_format, "TIDX tile keys are invalid, duplicated, or unsorted", path,
                tidx.offset + record_offset);
        }
        const std::uint32_t flags = read_u32(record, tile_index_offset::flags);
        const std::uint8_t child_mask = read_u8(record, tile_index_offset::child_mask);
        const std::uint16_t reserved = read_u16(record, tile_index_offset::reserved);
        if ((flags & ~0x00000003U) != 0 || (child_mask & 0xF0U) != 0 || reserved != 0) {
            return failure<std::vector<TileIndexEntry>>(
                ErrorCode::unsupported_feature, "TIDX flags, child mask, or reserved bytes are invalid", path,
                tidx.offset + record_offset + tile_index_offset::flags);
        }

        TileIndexEntry entry{key.value()};
        entry.pack_id = PackId{read_u32(record, tile_index_offset::pack_id)};
        entry.flags = flags;
        entry.payload_offset = read_u64(record, tile_index_offset::payload_offset);
        entry.stored_bytes = read_u32(record, tile_index_offset::stored_bytes);
        entry.logical_channel_bytes = read_u32(record, tile_index_offset::logical_bytes);
        entry.minimum_elevation_code = read_u16(record, tile_index_offset::minimum_elevation);
        entry.maximum_elevation_code = read_u16(record, tile_index_offset::maximum_elevation);
        entry.primary_dataset = DatasetId{read_u32(record, tile_index_offset::primary_dataset)};
        entry.effective_resolution_millimeters =
            read_u32(record, tile_index_offset::effective_resolution);
        entry.geometric_error_millimeters = read_u32(record, tile_index_offset::geometric_error);
        entry.materialized_child_mask = child_mask;
        entry.channel_count = read_u8(record, tile_index_offset::channel_count);
        entry.payload_crc32c = read_u32(record, tile_index_offset::payload_crc32c);
        std::copy_n(
            record.begin() + static_cast<std::ptrdiff_t>(tile_index_offset::content_hash),
            entry.content_hash_prefix.size(),
            entry.content_hash_prefix.begin());
        std::copy_n(
            record.begin() + static_cast<std::ptrdiff_t>(tile_index_offset::dependency_hash),
            entry.dependency_hash_prefix.size(),
            entry.dependency_hash_prefix.begin());

        const PackInfo* pack = find_pack(packs, entry.pack_id);
        if (pack == nullptr || entry.payload_offset < format_v1::bytes::pack_header || entry.payload_offset % 8U != 0 ||
            entry.stored_bytes == 0 ||
            !range_fits(entry.payload_offset, entry.stored_bytes, pack->file_bytes) ||
            entry.minimum_elevation_code > entry.maximum_elevation_code || entry.channel_count == 0 ||
            datasets.find(entry.primary_dataset.value) == datasets.end() ||
            entry.key.level() > header.maximum_level) {
            return failure<std::vector<TileIndexEntry>>(
                ErrorCode::invalid_format, "TIDX record fields or pack range are invalid", path,
                tidx.offset + record_offset);
        }
        entries.push_back(entry);
    }

    for (const PackInfo& pack : packs) {
        std::vector<const TileIndexEntry*> pack_entries;
        for (const auto& entry : entries) {
            if (entry.pack_id == pack.id) {
                pack_entries.push_back(&entry);
            }
        }
        if (pack_entries.size() != pack.tile_count || pack_entries.empty() ||
            pack_entries.front()->key != pack.first_key || pack_entries.back()->key != pack.last_key) {
            return failure<std::vector<TileIndexEntry>>(
                ErrorCode::invalid_format, "PACK registry tile bounds disagree with TIDX", path);
        }
        std::ranges::sort(pack_entries, {}, &TileIndexEntry::payload_offset);
        std::uint64_t previous_end = format_v1::bytes::pack_header;
        for (const TileIndexEntry* entry : pack_entries) {
            if (entry->payload_offset < previous_end) {
                return failure<std::vector<TileIndexEntry>>(
                    ErrorCode::invalid_format, "TIDX payload ranges overlap", path);
            }
            previous_end = entry->payload_offset + entry->stored_bytes;
        }
    }
    return Result<std::vector<TileIndexEntry>>::success(std::move(entries));
}

[[nodiscard]] Result<Sha256Digest> tile_index_hash(const ChunkInfo& tidx) {
    DigestAccumulator hash;
    hash.update_domain("LTDB_TILE_INDEX_V1");
    hash.update_u64(tidx.bytes.size());
    hash.update(tidx.bytes);
    return hash.finish();
}

[[nodiscard]] Result<Sha256Digest> database_content_hash(
    const std::vector<ChunkInfo>& chunks,
    const DatabaseHeader& header,
    const std::vector<PackInfo>& packs) {
    const std::uint32_t identity_count = static_cast<std::uint32_t>(std::ranges::count_if(
        chunks, [](const ChunkInfo& chunk) { return (chunk.flags & chunk_content_identity) != 0; }));
    DigestAccumulator hash;
    hash.update_domain("LTDB_DATABASE_CONTENT_V1");
    hash.update_u16(header.major_version);
    hash.update_u16(header.minor_version);
    hash.update(header.builder_configuration_hash.bytes);
    hash.update_u32(identity_count);
    for (const ChunkInfo& chunk : chunks) {
        if ((chunk.flags & chunk_content_identity) == 0) {
            continue;
        }
        hash.update(chunk.tag);
        hash.update_u16(chunk.version);
        hash.update_u16(chunk.flags);
        hash.update_u64(chunk.logical_bytes);
        hash.update(chunk.bytes);
    }
    hash.update_u32(static_cast<std::uint32_t>(packs.size()));
    for (const PackInfo& pack : packs) {
        hash.update_u32(pack.id.value);
        hash.update(pack.hash.bytes);
    }
    return hash.finish();
}

[[nodiscard]] Result<DatabaseState> parse_database(
    const std::filesystem::path& path,
    const Bytes file) {
    auto header = parse_header(file, path);
    if (!header) {
        return Result<DatabaseState>::failure(std::move(header).error());
    }
    auto chunks = parse_chunks(file, header.value(), path);
    if (!chunks) {
        return Result<DatabaseState>::failure(std::move(chunks).error());
    }
    auto strings = parse_strings(find_chunk(chunks.value(), "STRS"), path);
    if (!strings) {
        return Result<DatabaseState>::failure(std::move(strings).error());
    }

    std::set<std::uint32_t> dataset_ids;
    auto registry_hash = validate_datasets(
        find_chunk(chunks.value(), "DSET"),
        find_chunk(chunks.value(), "META"),
        strings.value(),
        header.value().header,
        path,
        dataset_ids);
    if (!registry_hash) {
        return Result<DatabaseState>::failure(std::move(registry_hash).error());
    }
    if (registry_hash.value() != header.value().header.dataset_registry_hash) {
        return failure<DatabaseState>(
            ErrorCode::hash_mismatch, "LTDB dataset-registry hash does not match", path, 128);
    }

    auto packs = parse_packs(
        find_chunk(chunks.value(), "PACK"), strings.value(), header.value().header, path);
    if (!packs) {
        return Result<DatabaseState>::failure(std::move(packs).error());
    }
    auto index = parse_tile_index(
        find_chunk(chunks.value(), "TIDX"),
        header.value().header,
        packs.value(),
        dataset_ids,
        path);
    if (!index) {
        return Result<DatabaseState>::failure(std::move(index).error());
    }

    auto computed_index_hash = tile_index_hash(find_chunk(chunks.value(), "TIDX"));
    if (!computed_index_hash) {
        return Result<DatabaseState>::failure(std::move(computed_index_hash).error());
    }
    if (computed_index_hash.value() != header.value().header.tile_index_hash) {
        return failure<DatabaseState>(
            ErrorCode::hash_mismatch, "LTDB tile-index hash does not match", path, 160);
    }
    auto computed_content_hash =
        database_content_hash(chunks.value(), header.value().header, packs.value());
    if (!computed_content_hash) {
        return Result<DatabaseState>::failure(std::move(computed_content_hash).error());
    }
    if (computed_content_hash.value() != header.value().header.database_content_hash) {
        return failure<DatabaseState>(
            ErrorCode::hash_mismatch, "LTDB database-content hash does not match", path, 192);
    }

    DigestAccumulator database_id_hash;
    database_id_hash.update_domain("LTDB_DATABASE_ID_V1");
    database_id_hash.update(header.value().header.builder_configuration_hash.bytes);
    database_id_hash.update(header.value().header.dataset_registry_hash.bytes);
    auto computed_database_id = database_id_hash.finish();
    if (!computed_database_id) {
        return Result<DatabaseState>::failure(std::move(computed_database_id).error());
    }
    if (!std::equal(
            header.value().header.database_id.bytes.begin(),
            header.value().header.database_id.bytes.end(),
            computed_database_id.value().bytes.begin())) {
        return failure<DatabaseState>(
            ErrorCode::hash_mismatch, "LTDB deterministic database ID does not match", path, 32);
    }

    return Result<DatabaseState>::success(DatabaseState{
        path,
        header.value().header,
        std::move(index).value(),
        std::move(packs).value(),
        std::move(dataset_ids),
    });
}

[[nodiscard]] Result<std::vector<std::byte>> decode_channel(
    const ChannelRecord& record,
    const Bytes stored,
    const std::filesystem::path& path,
    const std::uint64_t absolute_offset,
    const std::uint64_t tile_key) {
    Result<std::vector<std::byte>> logical = Result<std::vector<std::byte>>::failure(
        make_error(ErrorCode::internal_error, "channel decode was not selected", path));
    if (record.codec == static_cast<std::uint8_t>(Codec::none)) {
        if (record.stored_bytes != record.logical_bytes || record.parameter0 != 0) {
            return failure<std::vector<std::byte>>(
                ErrorCode::invalid_format, "uncompressed channel framing is invalid", path,
                absolute_offset);
        }
        logical = Result<std::vector<std::byte>>::success(
            std::vector<std::byte>{stored.begin(), stored.end()});
    } else if (record.codec == static_cast<std::uint8_t>(Codec::zstandard)) {
        logical = decompress_zstandard(stored, record.logical_bytes);
    } else {
        return failure<std::vector<std::byte>>(
            ErrorCode::unsupported_feature, "channel codec is not supported", path, absolute_offset);
    }
    if (!logical) {
        Error error = std::move(logical).error();
        error.with_path(path.string()).with_offset(absolute_offset).with_tile_key(tile_key).with_channel(record.id);
        return Result<std::vector<std::byte>>::failure(std::move(error));
    }

    if (record.predictor == static_cast<std::uint8_t>(Predictor::none)) {
        return logical;
    }
    if (record.predictor == static_cast<std::uint8_t>(Predictor::delta2d_u16)) {
        auto decoded = reverse_delta2d_u16(logical.value(), record.width, record.height);
        if (!decoded) {
            Error error = std::move(decoded).error();
            error.with_path(path.string()).with_offset(absolute_offset).with_tile_key(tile_key).with_channel(record.id);
            return Result<std::vector<std::byte>>::failure(std::move(error));
        }
        return decoded;
    }
    return failure<std::vector<std::byte>>(
        ErrorCode::unsupported_feature, "channel predictor is not supported", path, absolute_offset);
}

[[nodiscard]] Result<TileProvenance> parse_provenance(
    const ChannelRecord& record,
    const DatasetId primary_dataset,
    const std::filesystem::path& path,
    const std::uint64_t absolute_offset,
    const std::uint64_t tile_key) {
    using namespace format_v1;
    const Bytes bytes = record.decoded;
    if (bytes.size() < format_v1::bytes::provenance_header) {
        return failure<TileProvenance>(
            ErrorCode::truncated_data, "PRVN header is truncated", path, absolute_offset);
    }
    const std::uint16_t version = read_u16(bytes, provenance_header_offset::version);
    const std::uint16_t palette_count = read_u16(bytes, provenance_header_offset::palette_count);
    const std::uint16_t map_width = read_u16(bytes, provenance_header_offset::map_width);
    const std::uint16_t map_height = read_u16(bytes, provenance_header_offset::map_height);
    const std::uint8_t index_width = read_u8(bytes, provenance_header_offset::index_width);
    const std::uint16_t flags = read_u16(bytes, provenance_header_offset::flags);
    if (version != 1 || palette_count == 0 ||
        read_u8(bytes, provenance_header_offset::reserved0) != 0 ||
        read_u32(bytes, provenance_header_offset::reserved1) != 0 || (flags & ~0x1U) != 0) {
        return failure<TileProvenance>(
            ErrorCode::invalid_format, "PRVN header fields are invalid", path, absolute_offset);
    }
    const bool has_map = (flags & 0x1U) != 0;
    if ((!has_map && (map_width != 0 || map_height != 0 || index_width != 0 || flags != 0)) ||
        (has_map && (map_width == 0 || map_height == 0 ||
                     (index_width != 1 && index_width != 2) ||
                     (palette_count <= 256U ? index_width != 1 : index_width != 2)))) {
        return failure<TileProvenance>(
            ErrorCode::invalid_format, "PRVN map framing is invalid", path, absolute_offset);
    }

    const std::uint64_t palette_bytes =
        std::uint64_t{palette_count} * format_v1::bytes::provenance_palette_entry;
    const std::uint64_t map_samples = std::uint64_t{map_width} * map_height;
    const std::uint64_t map_bytes = map_samples * index_width;
    const std::uint64_t expected_bytes = format_v1::bytes::provenance_header + palette_bytes + map_bytes;
    if (expected_bytes != bytes.size()) {
        return failure<TileProvenance>(
            ErrorCode::invalid_format, "PRVN byte count disagrees with its header", path,
            absolute_offset);
    }

    TileProvenance provenance;
    provenance.map_width = map_width;
    provenance.map_height = map_height;
    provenance.palette.reserve(palette_count);
    std::uint32_t previous_dataset = 0;
    bool first = true;
    bool primary_present = false;
    double contribution_sum = 0.0;
    for (std::uint16_t index = 0; index < palette_count; ++index) {
        const std::size_t offset = format_v1::bytes::provenance_header +
                                   std::size_t{index} * format_v1::bytes::provenance_palette_entry;
        const std::uint32_t dataset = read_u32(bytes, offset);
        const std::uint32_t entry_flags = read_u32(bytes, offset + 4U);
        const float contribution = read_f32(bytes, offset + 8U);
        const float resolution = read_f32(bytes, offset + 12U);
        if ((!first && dataset <= previous_dataset) || entry_flags != 0 ||
            !std::isfinite(contribution) || contribution < 0.0F || contribution > 1.0F ||
            !std::isfinite(resolution) || resolution <= 0.0F) {
            Error error = make_error(
                ErrorCode::invalid_format, "PRVN palette entry is invalid", path,
                absolute_offset + offset);
            error.with_tile_key(tile_key).with_channel(record.id);
            return Result<TileProvenance>::failure(std::move(error));
        }
        first = false;
        previous_dataset = dataset;
        primary_present = primary_present || dataset == primary_dataset.value;
        contribution_sum += static_cast<double>(contribution);
        provenance.palette.push_back(ProvenancePaletteEntry{
            DatasetId{dataset}, entry_flags, contribution, resolution});
    }
    if (!primary_present || std::abs(contribution_sum - 1.0) > 1.0e-6) {
        return failure<TileProvenance>(
            ErrorCode::invalid_format, "PRVN palette omits the primary dataset or has invalid weights", path,
            absolute_offset);
    }

    provenance.dominant_source_indices.reserve(static_cast<std::size_t>(map_samples));
    std::size_t map_offset = format_v1::bytes::provenance_header + static_cast<std::size_t>(palette_bytes);
    for (std::uint64_t sample = 0; sample < map_samples; ++sample) {
        std::uint16_t palette_index = read_u8(bytes, map_offset);
        if (index_width == 2) {
            palette_index = read_u16(bytes, map_offset);
        }
        if (palette_index >= palette_count) {
            return failure<TileProvenance>(
                ErrorCode::invalid_format, "PRVN map index is outside the palette", path,
                absolute_offset + map_offset);
        }
        provenance.dominant_source_indices.push_back(palette_index);
        map_offset += index_width;
    }
    return Result<TileProvenance>::success(std::move(provenance));
}

[[nodiscard]] Result<DecodedTerrainTile> decode_tile(
    const Bytes payload,
    const TileIndexEntry& entry,
    const DatabaseHeader& database_header,
    const std::filesystem::path& pack_path) {
    using namespace format_v1;
    const std::uint64_t tile_absolute_offset = entry.payload_offset;
    if (payload.size() < format_v1::bytes::tile_header) {
        return failure<DecodedTerrainTile>(
            ErrorCode::truncated_data, "LTIL header is truncated", pack_path, tile_absolute_offset);
    }
    if (!magic_is(payload, tile_header_offset::magic, "LTIL") ||
        read_u16(payload, tile_header_offset::version) != 1 ||
        read_u16(payload, tile_header_offset::header_bytes) != format_v1::bytes::tile_header) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL magic, version, or header size is invalid", pack_path,
            tile_absolute_offset);
    }
    if (read_u64(payload, tile_header_offset::tile_key) != entry.key.encoded()) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL TileKey disagrees with TIDX", pack_path,
            tile_absolute_offset + tile_header_offset::tile_key);
    }

    const std::uint32_t flags = read_u32(payload, tile_header_offset::flags);
    const std::uint16_t channel_count = read_u16(payload, tile_header_offset::channel_count);
    const std::uint16_t stored_tile_cells = read_u16(payload, tile_header_offset::tile_cells);
    const std::uint16_t stored_core_vertices = read_u16(payload, tile_header_offset::core_vertices);
    const std::uint8_t apron = read_u8(payload, tile_header_offset::apron);
    const std::uint8_t encoding_profile = read_u8(payload, tile_header_offset::encoding_profile);
    const float effective_resolution = read_f32(payload, tile_header_offset::effective_resolution);
    const float geometric_error = read_f32(payload, tile_header_offset::geometric_error);
    const float minimum_elevation = read_f32(payload, tile_header_offset::minimum_elevation);
    const float maximum_elevation = read_f32(payload, tile_header_offset::maximum_elevation);
    const DatasetId primary_dataset{read_u32(payload, tile_header_offset::primary_dataset)};
    const std::uint16_t palette_count =
        read_u16(payload, tile_header_offset::provenance_palette_count);
    const std::uint32_t directory_offset =
        read_u32(payload, tile_header_offset::channel_directory_offset);
    const std::uint32_t directory_bytes =
        read_u32(payload, tile_header_offset::channel_directory_bytes);
    const std::uint32_t data_offset = read_u32(payload, tile_header_offset::data_region_offset);
    const std::uint32_t data_bytes = read_u32(payload, tile_header_offset::data_region_bytes);

    if ((flags & ~0x00000003U) != 0 || flags != entry.flags || channel_count != entry.channel_count ||
        stored_tile_cells != format_v1::tile_cells ||
        stored_core_vertices != format_v1::core_vertices ||
        apron != format_v1::apron_samples || encoding_profile != static_cast<std::uint8_t>(EncodingProfile::global_u16) ||
        primary_dataset != entry.primary_dataset || read_u16(payload, tile_header_offset::reserved0) != 0 ||
        !all_zero(payload.subspan(tile_header_offset::reserved1, 28)) ||
        !std::isfinite(effective_resolution) || effective_resolution <= 0.0F ||
        !std::isfinite(geometric_error) || geometric_error < 0.0F ||
        !std::isfinite(minimum_elevation) || !std::isfinite(maximum_elevation) ||
        minimum_elevation > maximum_elevation) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL fixed fields disagree with TIDX or format v1", pack_path,
            tile_absolute_offset + 16U);
    }
    if (directory_offset != format_v1::bytes::tile_header ||
        directory_bytes != std::uint64_t{channel_count} * format_v1::bytes::channel_record ||
        data_offset != align8(std::uint64_t{directory_offset} + directory_bytes) ||
        !range_fits(directory_offset, directory_bytes, payload.size()) ||
        !range_fits(data_offset, data_bytes, payload.size()) ||
        std::uint64_t{data_offset} + data_bytes != payload.size() ||
        !all_zero(payload.subspan(directory_offset + directory_bytes, data_offset - directory_offset - directory_bytes))) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL channel directory or data-region framing is invalid", pack_path,
            tile_absolute_offset + tile_header_offset::channel_directory_offset);
    }
    if (!std::equal(
            payload.begin() + static_cast<std::ptrdiff_t>(tile_header_offset::content_hash),
            payload.begin() + static_cast<std::ptrdiff_t>(tile_header_offset::content_hash + 16U),
            entry.content_hash_prefix.begin()) ||
        !std::equal(
            payload.begin() + static_cast<std::ptrdiff_t>(tile_header_offset::dependency_hash),
            payload.begin() + static_cast<std::ptrdiff_t>(tile_header_offset::dependency_hash + 8U),
            entry.dependency_hash_prefix.begin())) {
        return failure<DecodedTerrainTile>(
            ErrorCode::hash_mismatch, "LTIL hash prefixes disagree with TIDX", pack_path,
            tile_absolute_offset + tile_header_offset::dependency_hash);
    }

    std::vector<ChannelRecord> records;
    records.reserve(channel_count);
    std::uint16_t previous_id = 0;
    std::uint64_t previous_data_end = data_offset;
    std::uint64_t logical_sum = 0;
    bool saw_elevation = false;
    bool saw_provenance = false;
    bool saw_quality = false;
    for (std::uint16_t index = 0; index < channel_count; ++index) {
        const std::size_t record_offset = directory_offset +
                                          std::size_t{index} * format_v1::bytes::channel_record;
        const Bytes raw = payload.subspan(record_offset, format_v1::bytes::channel_record);
        ChannelRecord record;
        record.id = read_u16(raw, channel_record_offset::channel_id);
        record.version = read_u16(raw, channel_record_offset::version);
        record.element_type = read_u8(raw, channel_record_offset::element_type);
        record.components = read_u8(raw, channel_record_offset::components);
        record.codec = read_u8(raw, channel_record_offset::codec);
        record.predictor = read_u8(raw, channel_record_offset::predictor);
        record.width = read_u16(raw, channel_record_offset::width);
        record.height = read_u16(raw, channel_record_offset::height);
        record.flags = read_u32(raw, channel_record_offset::flags);
        record.data_offset = read_u32(raw, channel_record_offset::data_offset);
        record.stored_bytes = read_u32(raw, channel_record_offset::stored_bytes);
        record.logical_bytes = read_u32(raw, channel_record_offset::logical_bytes);
        record.crc = read_u32(raw, channel_record_offset::crc32c);
        record.parameter0 = read_u32(raw, channel_record_offset::parameter0);
        record.parameter1 = read_u32(raw, channel_record_offset::parameter1);

        if ((index > 0 && record.id <= previous_id) || (record.flags & ~0x1U) != 0 ||
            record.data_offset % 8U != 0 || record.data_offset < previous_data_end ||
            record.data_offset < data_offset ||
            !range_fits(record.data_offset, record.stored_bytes, payload.size()) ||
            !all_zero(payload.subspan(
                static_cast<std::size_t>(previous_data_end),
                static_cast<std::size_t>(record.data_offset - previous_data_end)))) {
            return failure<DecodedTerrainTile>(
                ErrorCode::invalid_format, "LTIL channel records are unsorted, overlapping, or misaligned", pack_path,
                tile_absolute_offset + record_offset);
        }
        previous_id = record.id;
        previous_data_end = std::uint64_t{record.data_offset} + record.stored_bytes;
        if (record.logical_bytes > std::numeric_limits<std::uint64_t>::max() - logical_sum) {
            return failure<DecodedTerrainTile>(
                ErrorCode::arithmetic_overflow, "LTIL logical channel byte sum overflowed", pack_path,
                tile_absolute_offset + record_offset);
        }
        logical_sum += record.logical_bytes;

        const Bytes stored = payload.subspan(record.data_offset, record.stored_bytes);
        if (crc32c(stored) != record.crc) {
            Error error = make_error(
                ErrorCode::checksum_mismatch, "channel CRC32C does not match", pack_path,
                tile_absolute_offset + record.data_offset);
            error.with_tile_key(entry.key.encoded()).with_channel(record.id);
            return Result<DecodedTerrainTile>::failure(std::move(error));
        }

        const bool known = record.id == static_cast<std::uint16_t>(ChannelId::elevation) ||
                           record.id == static_cast<std::uint16_t>(ChannelId::provenance) ||
                           record.id == static_cast<std::uint16_t>(ChannelId::quality);
        if (!known) {
            if ((record.flags & channel_required) != 0) {
                return failure<DecodedTerrainTile>(
                    ErrorCode::unsupported_feature, "unknown required channel is not supported", pack_path,
                    tile_absolute_offset + record_offset);
            }
            records.push_back(std::move(record));
            continue;
        }
        if (record.version != 1 || record.components != 1) {
            return failure<DecodedTerrainTile>(
                record.version != 1 ? ErrorCode::unsupported_version : ErrorCode::invalid_format,
                "known channel version or component count is invalid", pack_path,
                tile_absolute_offset + record_offset);
        }
        auto decoded = decode_channel(
            record, stored, pack_path, tile_absolute_offset + record.data_offset, entry.key.encoded());
        if (!decoded) {
            return Result<DecodedTerrainTile>::failure(std::move(decoded).error());
        }
        record.decoded = std::move(decoded).value();

        if (record.id == static_cast<std::uint16_t>(ChannelId::elevation)) {
            saw_elevation = true;
            const std::uint64_t required_bytes = std::uint64_t{record.width} * record.height * 2U;
            if (record.element_type != static_cast<std::uint8_t>(ElementType::u16) ||
                record.predictor != static_cast<std::uint8_t>(Predictor::delta2d_u16) ||
                record.flags != channel_required ||
                record.parameter1 != static_cast<std::uint32_t>(QuantizationId::global_u16_0p5m) ||
                required_bytes != record.decoded.size()) {
                return failure<DecodedTerrainTile>(
                    ErrorCode::invalid_format, "ELEV channel contract is invalid", pack_path,
                    tile_absolute_offset + record_offset);
            }
        } else if (record.id == static_cast<std::uint16_t>(ChannelId::provenance)) {
            saw_provenance = true;
            if (record.element_type != static_cast<std::uint8_t>(ElementType::opaque) ||
                record.predictor != static_cast<std::uint8_t>(Predictor::none) ||
                record.flags != channel_required || record.parameter1 != 0) {
                return failure<DecodedTerrainTile>(
                    ErrorCode::invalid_format, "PRVN channel contract is invalid", pack_path,
                    tile_absolute_offset + record_offset);
            }
        } else {
            saw_quality = true;
            const std::uint64_t required_bytes = std::uint64_t{record.width} * record.height;
            if (record.element_type != static_cast<std::uint8_t>(ElementType::u8) ||
                record.predictor != static_cast<std::uint8_t>(Predictor::none) || record.flags != 0 ||
                record.parameter1 != 0 || required_bytes != record.decoded.size() ||
                std::ranges::any_of(record.decoded, [](const std::byte value) {
                    return (std::to_integer<std::uint8_t>(value) & 0xE0U) != 0;
                })) {
                return failure<DecodedTerrainTile>(
                    ErrorCode::invalid_format, "QUAL channel contract is invalid", pack_path,
                    tile_absolute_offset + record_offset);
            }
        }
        records.push_back(std::move(record));
    }
    if (previous_data_end != payload.size() || logical_sum != entry.logical_channel_bytes ||
        !saw_elevation || !saw_provenance || saw_provenance != ((flags & 0x1U) != 0) ||
        saw_quality != ((flags & 0x2U) != 0)) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL required channels, flags, or byte totals disagree", pack_path,
            tile_absolute_offset);
    }

    DigestAccumulator content_hash;
    content_hash.update_domain("LTDB_TILE_CONTENT_V1");
    content_hash.update_u64(entry.key.encoded());
    content_hash.update_u16(channel_count);
    bool can_validate_content_hash = true;
    for (const ChannelRecord& record : records) {
        if (record.decoded.empty() && record.logical_bytes != 0) {
            can_validate_content_hash = false;
            continue;
        }
        content_hash.update_u16(record.id);
        content_hash.update_u16(record.version);
        content_hash.update_u8(record.element_type);
        content_hash.update_u8(record.components);
        content_hash.update_u8(record.predictor);
        content_hash.update_u8(0);
        content_hash.update_u16(record.width);
        content_hash.update_u16(record.height);
        content_hash.update_u32(record.flags);
        content_hash.update_u32(static_cast<std::uint32_t>(record.decoded.size()));
        content_hash.update_u32(0);
        content_hash.update_u32(record.parameter1);
        content_hash.update(record.decoded);
    }
    if (can_validate_content_hash) {
        auto computed = content_hash.finish();
        if (!computed) {
            return Result<DecodedTerrainTile>::failure(std::move(computed).error());
        }
        const Bytes stored_prefix = payload.subspan(tile_header_offset::content_hash, 16);
        if (!digest_prefix_matches(computed.value(), stored_prefix)) {
            return failure<DecodedTerrainTile>(
                ErrorCode::hash_mismatch, "LTIL decoded content hash does not match", pack_path,
                tile_absolute_offset + tile_header_offset::content_hash);
        }
    }

    std::vector<DecodedChannel> channels;
    channels.reserve(records.size());
    std::optional<TileProvenance> provenance;
    std::uint16_t minimum_code = std::numeric_limits<std::uint16_t>::max();
    std::uint16_t maximum_code = 0;
    for (ChannelRecord& record : records) {
        if (record.decoded.empty() && record.logical_bytes != 0) {
            continue;
        }
        if (record.id == static_cast<std::uint16_t>(ChannelId::elevation)) {
            for (std::size_t offset = 0; offset < record.decoded.size(); offset += 2U) {
                const std::uint16_t value = read_u16(record.decoded, offset);
                minimum_code = std::min(minimum_code, value);
                maximum_code = std::max(maximum_code, value);
            }
        } else if (record.id == static_cast<std::uint16_t>(ChannelId::provenance)) {
            auto parsed = parse_provenance(
                record,
                primary_dataset,
                pack_path,
                tile_absolute_offset + record.data_offset,
                entry.key.encoded());
            if (!parsed) {
                return Result<DecodedTerrainTile>::failure(std::move(parsed).error());
            }
            if (parsed.value().palette.size() != palette_count ||
                parsed.value().map_width != record.width || parsed.value().map_height != record.height) {
                return failure<DecodedTerrainTile>(
                    ErrorCode::invalid_format, "PRVN channel disagrees with LTIL summary", pack_path,
                    tile_absolute_offset + record.data_offset);
            }
            provenance = std::move(parsed).value();
        }

        channels.emplace_back(
            static_cast<ChannelId>(record.id),
            record.version,
            static_cast<ElementType>(record.element_type),
            record.components,
            record.width,
            record.height,
            record.flags,
            record.parameter1,
            std::move(record.decoded));
    }

    const auto rounded_millimeters = [](const float meters) -> std::optional<std::uint32_t> {
        const double millimeters = std::round(static_cast<double>(meters) * 1000.0);
        if (millimeters < 0.0 || millimeters > std::numeric_limits<std::uint32_t>::max()) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(millimeters);
    };
    const auto effective_mm = rounded_millimeters(effective_resolution);
    const auto error_mm = rounded_millimeters(geometric_error);
    const float expected_minimum = static_cast<float>(
        database_header.elevation_origin_meters +
        static_cast<double>(minimum_code) * database_header.elevation_step_meters);
    const float expected_maximum = static_cast<float>(
        database_header.elevation_origin_meters +
        static_cast<double>(maximum_code) * database_header.elevation_step_meters);
    if (minimum_code != entry.minimum_elevation_code || maximum_code != entry.maximum_elevation_code ||
        minimum_elevation != expected_minimum || maximum_elevation != expected_maximum ||
        !effective_mm || *effective_mm != entry.effective_resolution_millimeters ||
        !error_mm || *error_mm != entry.geometric_error_millimeters) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTIL scientific summaries disagree with decoded content or TIDX", pack_path,
            tile_absolute_offset + tile_header_offset::effective_resolution);
    }

    return Result<DecodedTerrainTile>::success(DecodedTerrainTile{
        entry.key,
        DecodedTileMetadata{
            effective_resolution,
            geometric_error,
            minimum_elevation,
            maximum_elevation,
            primary_dataset,
        },
        std::move(channels),
        std::move(provenance),
    });
}

[[nodiscard]] Result<DecodedTerrainTile> read_tile(
    const DatabaseState& state,
    const TileIndexEntry& entry) {
    using namespace format_v1;
    const PackInfo* pack = find_pack(state.packs, entry.pack_id);
    if (pack == nullptr) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "TIDX references a missing pack", state.manifest_path);
    }
    const std::filesystem::path pack_path = state.manifest_path.parent_path() / pack->relative_path;
    auto file = read_file(pack_path);
    if (!file) {
        Error error = std::move(file).error();
        error.with_tile_key(entry.key.encoded());
        return Result<DecodedTerrainTile>::failure(std::move(error));
    }
    const Bytes bytes = file.value();
    if (bytes.size() < format_v1::bytes::pack_header || !magic_is(bytes, pack_header_offset::magic, "LTPK")) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTP pack header is missing or invalid", pack_path, 0);
    }
    const std::uint16_t major = read_u16(bytes, pack_header_offset::major);
    if (major != major_version) {
        return failure<DecodedTerrainTile>(
            ErrorCode::unsupported_version, "LTP major version is not supported", pack_path, 4);
    }
    if (read_u32(bytes, pack_header_offset::header_bytes) != format_v1::bytes::pack_header ||
        read_u32(bytes, pack_header_offset::endian) != endian_tag ||
        read_u32(bytes, pack_header_offset::pack_id) != pack->id.value ||
        read_u32(bytes, pack_header_offset::flags) != 0 ||
        read_u64(bytes, pack_header_offset::tile_count) != pack->tile_count ||
        read_u64(bytes, pack_header_offset::payload_region_offset) != format_v1::bytes::pack_header ||
        read_u64(bytes, pack_header_offset::file_bytes) != bytes.size() ||
        bytes.size() != pack->file_bytes) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTP header fields disagree with the PACK registry", pack_path, 8);
    }

    DigestAccumulator pack_hash;
    pack_hash.update(bytes.first(pack_header_offset::sha256_prefix));
    const std::array<std::byte, 16> zero_hash{};
    pack_hash.update(zero_hash);
    pack_hash.update(bytes.subspan(format_v1::bytes::pack_header));
    auto computed_pack_hash = pack_hash.finish();
    if (!computed_pack_hash) {
        return Result<DecodedTerrainTile>::failure(std::move(computed_pack_hash).error());
    }
    if (computed_pack_hash.value() != pack->hash ||
        !digest_prefix_matches(
            computed_pack_hash.value(), bytes.subspan(pack_header_offset::sha256_prefix, 16))) {
        return failure<DecodedTerrainTile>(
            ErrorCode::hash_mismatch, "LTP full hash or stored hash prefix does not match", pack_path, 48);
    }

    std::uint64_t previous_payload_end = format_v1::bytes::pack_header;
    for (const TileIndexEntry& indexed : state.index) {
        if (indexed.pack_id != pack->id) {
            continue;
        }
        if (indexed.payload_offset < previous_payload_end ||
            !range_fits(indexed.payload_offset, indexed.stored_bytes, bytes.size()) ||
            !all_zero(bytes.subspan(
                static_cast<std::size_t>(previous_payload_end),
                static_cast<std::size_t>(indexed.payload_offset - previous_payload_end)))) {
            return failure<DecodedTerrainTile>(
                ErrorCode::invalid_format, "LTP payload ranges or alignment bytes are invalid", pack_path,
                previous_payload_end);
        }
        previous_payload_end = indexed.payload_offset + indexed.stored_bytes;
    }
    if (previous_payload_end != bytes.size()) {
        return failure<DecodedTerrainTile>(
            ErrorCode::invalid_format, "LTP has trailing bytes outside its final tile payload", pack_path,
            previous_payload_end);
    }
    if (!range_fits(entry.payload_offset, entry.stored_bytes, bytes.size())) {
        return failure<DecodedTerrainTile>(
            ErrorCode::truncated_data, "TIDX tile payload range is outside its pack", pack_path,
            entry.payload_offset);
    }
    const Bytes payload = bytes.subspan(entry.payload_offset, entry.stored_bytes);
    if (crc32c(payload) != entry.payload_crc32c) {
        Error error = make_error(
            ErrorCode::checksum_mismatch, "LTIL payload CRC32C does not match TIDX", pack_path,
            entry.payload_offset);
        error.with_tile_key(entry.key.encoded());
        return Result<DecodedTerrainTile>::failure(std::move(error));
    }
    return decode_tile(payload, entry, state.header, pack_path);
}

[[nodiscard]] bool in_subtree(const LunarTileKey candidate, const LunarTileKey root) noexcept {
    if (candidate.face() != root.face() || candidate.level() < root.level()) {
        return false;
    }
    const std::uint8_t level_difference =
        static_cast<std::uint8_t>(candidate.level() - root.level());
    return (candidate.morton() >> (2U * level_difference)) == root.morton();
}

}  // namespace

struct LunarTerrainDatabase::Impl {
    explicit Impl(DatabaseState value) : state(std::move(value)) {}
    DatabaseState state;
};

LunarTerrainDatabase::LunarTerrainDatabase(std::unique_ptr<Impl> implementation) noexcept
    : impl_(std::move(implementation)) {}

LunarTerrainDatabase::~LunarTerrainDatabase() = default;
LunarTerrainDatabase::LunarTerrainDatabase(LunarTerrainDatabase&&) noexcept = default;
LunarTerrainDatabase& LunarTerrainDatabase::operator=(LunarTerrainDatabase&&) noexcept = default;

Result<LunarTerrainDatabase> LunarTerrainDatabase::Open(const std::filesystem::path& path) {
    auto file = read_file(path);
    if (!file) {
        return Result<LunarTerrainDatabase>::failure(std::move(file).error());
    }
    auto parsed = parse_database(path, file.value());
    if (!parsed) {
        return Result<LunarTerrainDatabase>::failure(std::move(parsed).error());
    }
    return Result<LunarTerrainDatabase>::success(
        LunarTerrainDatabase{std::make_unique<Impl>(std::move(parsed).value())});
}

const DatabaseHeader& LunarTerrainDatabase::Header() const noexcept {
    return impl_->state.header;
}

std::optional<TileIndexEntry> LunarTerrainDatabase::FindTile(const LunarTileKey key) const {
    const auto found = std::ranges::lower_bound(
        impl_->state.index, key, {}, &TileIndexEntry::key);
    if (found == impl_->state.index.end() || found->key != key) {
        return std::nullopt;
    }
    return *found;
}

Result<DecodedTerrainTile> LunarTerrainDatabase::ReadTile(const LunarTileKey key) const {
    const auto entry = FindTile(key);
    if (!entry) {
        Error error{ErrorCode::not_found, "tile is not present in the sparse database index"};
        error.with_path(impl_->state.manifest_path.string()).with_tile_key(key.encoded());
        return Result<DecodedTerrainTile>::failure(std::move(error));
    }
    return read_tile(impl_->state, *entry);
}

std::vector<TileIndexEntry> LunarTerrainDatabase::Children(const LunarTileKey parent) const {
    std::vector<TileIndexEntry> result;
    auto child_keys = parent.children();
    if (!child_keys) {
        return result;
    }
    result.reserve(4);
    for (const LunarTileKey child : child_keys.value()) {
        auto entry = FindTile(child);
        if (entry) {
            result.push_back(*entry);
        }
    }
    return result;
}

std::vector<TileIndexEntry> LunarTerrainDatabase::QuerySubtree(const LunarTileKey root) const {
    std::vector<TileIndexEntry> result;
    for (const auto& entry : impl_->state.index) {
        if (in_subtree(entry.key, root)) {
            result.push_back(entry);
        }
    }
    return result;
}

std::vector<TileIndexEntry> LunarTerrainDatabase::QueryLeaves(const LunarTileKey root) const {
    std::vector<TileIndexEntry> result;
    for (const auto& entry : impl_->state.index) {
        if (entry.materialized_child_mask == 0 && in_subtree(entry.key, root)) {
            result.push_back(entry);
        }
    }
    return result;
}

}  // namespace lunar::terrain
