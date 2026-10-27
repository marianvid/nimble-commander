// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace nc::vfs::s3 {

// Thin wrappers over the platform crypto primitives required by AWS Signature Version 4.
using Sha256Digest = std::array<std::byte, 32>;

Sha256Digest Sha256(std::span<const std::byte> _data) noexcept;
Sha256Digest Sha256(std::string_view _data) noexcept;

Sha256Digest HmacSha256(std::span<const std::byte> _key, std::string_view _data) noexcept;

// Lowercase hexadecimal representation, as required by SigV4.
std::string ToHex(std::span<const std::byte> _bytes);

// Hex-encoded SHA256 of an empty payload, used for requests without a body.
inline constexpr std::string_view EmptyPayloadSha256 =
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855";

} // namespace nc::vfs::s3
