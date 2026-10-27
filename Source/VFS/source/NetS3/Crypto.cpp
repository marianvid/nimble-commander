// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Crypto.h"

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#else
// Non-Apple builds are only used for running the pure unit tests on a development host.
#include <openssl/evp.h>
#include <openssl/hmac.h>
#endif

namespace nc::vfs::s3 {

Sha256Digest Sha256(std::span<const std::byte> _data) noexcept
{
    Sha256Digest digest{};
#if defined(__APPLE__)
    CC_SHA256(_data.data(), static_cast<CC_LONG>(_data.size()), reinterpret_cast<unsigned char *>(digest.data()));
#else
    unsigned int len = 0;
    EVP_Digest(
        _data.data(), _data.size(), reinterpret_cast<unsigned char *>(digest.data()), &len, EVP_sha256(), nullptr);
#endif
    return digest;
}

Sha256Digest Sha256(std::string_view _data) noexcept
{
    return Sha256(std::as_bytes(std::span{_data.data(), _data.size()}));
}

Sha256Digest HmacSha256(std::span<const std::byte> _key, std::string_view _data) noexcept
{
    Sha256Digest mac{};
#if defined(__APPLE__)
    CCHmac(kCCHmacAlgSHA256, _key.data(), _key.size(), _data.data(), _data.size(), mac.data());
#else
    unsigned int len = 0;
    HMAC(EVP_sha256(),
         _key.data(),
         static_cast<int>(_key.size()),
         reinterpret_cast<const unsigned char *>(_data.data()),
         _data.size(),
         reinterpret_cast<unsigned char *>(mac.data()),
         &len);
#endif
    return mac;
}

std::string ToHex(std::span<const std::byte> _bytes)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(_bytes.size() * 2);
    for( const std::byte b : _bytes ) {
        const auto v = std::to_integer<unsigned>(b);
        hex.push_back(digits[v >> 4]);
        hex.push_back(digits[v & 0xF]);
    }
    return hex;
}

} // namespace nc::vfs::s3
