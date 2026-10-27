// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc::vfs::s3 {

// A user-specified endpoint of an S3-compatible service, e.g. "https://minio.lan:9000".
struct Endpoint {
    bool https = true;
    std::string host;
    int port = -1; // -1 means the default port of the scheme
    bool operator==(const Endpoint &) const noexcept = default;
};

// Accepts "https://host:port", "http://host", "host:port" or "host"; any path component is ignored.
// A missing scheme defaults to https. Returns nullopt on malformed input.
std::optional<Endpoint> ParseEndpoint(std::string_view _input);

// Where a request must go and what must be signed for it.
struct RequestTarget {
    std::string url;           // full URL including the query string
    std::string host_header;   // value of the Host header, "host" or "host:port"
    std::string canonical_uri; // URI-encoded path, as it appears in the URL and in the signature
};

// Builds request targets for AWS S3 or a custom S3-compatible endpoint.
// Virtual-hosted addressing ("bucket.host/key") is used for AWS unless the bucket name contains dots (which breaks
// TLS wildcard certificates) or path-style was requested explicitly. Custom endpoints honor the path-style flag.
class Addressing
{
public:
    Addressing(std::optional<Endpoint> _custom_endpoint, bool _path_style);

    // _bucket may be empty to address the service itself (ListBuckets).
    // _query contains raw (unencoded) parameters, they are encoded identically to the SigV4 canonical query.
    [[nodiscard]] RequestTarget Make(std::string_view _bucket,
                                     std::string_view _key,
                                     std::string_view _region,
                                     const std::vector<std::pair<std::string, std::string>> &_query) const;

    [[nodiscard]] bool IsAWS() const noexcept { return !m_Custom.has_value(); }

private:
    [[nodiscard]] bool UsePathStyle(std::string_view _bucket) const noexcept;

    std::optional<Endpoint> m_Custom;
    bool m_PathStyle;
};

} // namespace nc::vfs::s3
