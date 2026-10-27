// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <ctime>
#include <optional>
#include <string>
#include <string_view>

namespace nc::vfs::s3 {

// Percent-encodes everything except the RFC 3986 unreserved characters (A-Z a-z 0-9 - _ . ~).
// When _keep_slash is true, '/' is left as is - this is the form used for object keys inside URI paths.
std::string UriEncode(std::string_view _input, bool _keep_slash);

// Decodes a string produced by S3 with "encoding-type=url": percent-escapes are decoded and '+' becomes a space.
std::string FormUrlDecode(std::string_view _input);

// Formats a UNIX time as the ISO8601 basic format required by SigV4: "YYYYMMDD'T'HHMMSS'Z'".
std::string AmzDateTime(std::time_t _time);

// Parses S3 timestamps such as "2009-10-12T17:50:30.000Z" or "2009-10-12T17:50:30Z".
std::optional<std::time_t> ParseIso8601(std::string_view _input) noexcept;

// Parses RFC1123 HTTP dates such as "Wed, 12 Oct 2009 17:50:00 GMT" (used by the Last-Modified header).
std::optional<std::time_t> ParseHttpDate(std::string_view _input) noexcept;

// Removes leading and trailing whitespace.
std::string_view TrimWhitespace(std::string_view _input) noexcept;

} // namespace nc::vfs::s3
