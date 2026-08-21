#include <lunar/terrain/integrity.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

#include <openssl/evp.h>

#include <lunar/terrain/error.hpp>

namespace lunar::terrain {

std::uint32_t crc32c(const std::span<const std::byte> bytes) noexcept {
    std::uint32_t value = 0xFFFFFFFFU;
    for (const std::byte byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        for (std::uint8_t bit = 0; bit < 8; ++bit) {
            const std::uint32_t mask = 0U - (value & 1U);
            value = (value >> 1U) ^ (0x82F63B78U & mask);
        }
    }
    return value ^ 0xFFFFFFFFU;
}

Result<Sha256Digest> sha256(const std::span<const std::byte> bytes) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        return Result<Sha256Digest>::failure(
            Error{ErrorCode::internal_error, "OpenSSL could not allocate a SHA-256 context"});
    }

    Sha256Digest digest;
    unsigned int digest_bytes = 0;
    const bool initialized = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1;
    const bool updated = initialized && EVP_DigestUpdate(context, bytes.data(), bytes.size()) == 1;
    const bool finalized =
        updated && EVP_DigestFinal_ex(
                       context,
                       reinterpret_cast<unsigned char*>(digest.bytes.data()),
                       &digest_bytes) == 1;
    EVP_MD_CTX_free(context);

    if (!finalized || digest_bytes != digest.bytes.size()) {
        return Result<Sha256Digest>::failure(
            Error{ErrorCode::internal_error, "OpenSSL SHA-256 calculation failed"});
    }
    return Result<Sha256Digest>::success(digest);
}

}  // namespace lunar::terrain
