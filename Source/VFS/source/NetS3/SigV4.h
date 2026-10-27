// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc::vfs::s3 {

struct Credentials {
    std::string access_key;
    std::string secret_key;
    std::string session_token; // optional, set for temporary (STS) credentials
    bool operator==(const Credentials &) const noexcept = default;
};

// A request reduced to the parts that participate in the signature.
struct SignableRequest {
    std::string method;                                     // "GET", "PUT", ...
    std::string canonical_uri;                              // already URI-encoded path, e.g. "/bucket/my%20key"
    std::vector<std::pair<std::string, std::string>> query; // raw (unencoded) query parameters
    std::vector<std::pair<std::string, std::string>> headers; // every header listed here gets signed
};

// AWS Signature Version 4 for the "s3" service.
// Stateless apart from its immutable inputs, so it is safe to share across threads.
class SigV4Signer
{
public:
    SigV4Signer(Credentials _credentials, std::string _region, std::string _service = "s3");

    // Produces the value for the "Authorization" header.
    // _amz_date must be the same value that was placed into the "x-amz-date" header ("YYYYMMDDTHHMMSSZ").
    [[nodiscard]] std::string Authorization(const SignableRequest &_request,
                                            std::string_view _amz_date,
                                            std::string_view _payload_sha256_hex) const;

    // Intermediate artifacts, exposed for unit testing against the published AWS examples.
    [[nodiscard]] static std::string CanonicalQuery(const SignableRequest &_request);
    [[nodiscard]] static std::string CanonicalRequest(const SignableRequest &_request,
                                                      std::string_view _payload_sha256_hex);
    [[nodiscard]] static std::string SignedHeaders(const SignableRequest &_request);

    [[nodiscard]] const Credentials &GetCredentials() const noexcept { return m_Credentials; }
    [[nodiscard]] const std::string &Region() const noexcept { return m_Region; }

private:
    Credentials m_Credentials;
    std::string m_Region;
    std::string m_Service;
};

} // namespace nc::vfs::s3
