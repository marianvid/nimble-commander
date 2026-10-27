// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "SigV4.h"
#include "Crypto.h"
#include "Encoding.h"
#include <algorithm>
#include <cctype>

namespace nc::vfs::s3 {

static std::string ToLower(std::string_view _s)
{
    std::string r(_s);
    std::ranges::transform(r, r.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return r;
}

// Collapses sequential spaces into one and trims the value, as mandated by SigV4.
static std::string CanonicalHeaderValue(std::string_view _value)
{
    const std::string_view trimmed = TrimWhitespace(_value);
    std::string out;
    out.reserve(trimmed.size());
    bool prev_space = false;
    for( const char c : trimmed ) {
        const bool space = c == ' ' || c == '\t';
        if( space && prev_space )
            continue;
        out.push_back(space ? ' ' : c);
        prev_space = space;
    }
    return out;
}

static std::vector<std::pair<std::string, std::string>> SortedCanonicalHeaders(const SignableRequest &_request)
{
    std::vector<std::pair<std::string, std::string>> headers;
    headers.reserve(_request.headers.size());
    for( const auto &[name, value] : _request.headers )
        headers.emplace_back(ToLower(name), CanonicalHeaderValue(value));
    std::ranges::sort(headers, [](const auto &a, const auto &b) { return a.first < b.first; });
    return headers;
}

SigV4Signer::SigV4Signer(Credentials _credentials, std::string _region, std::string _service)
    : m_Credentials(std::move(_credentials)), m_Region(std::move(_region)), m_Service(std::move(_service))
{
}

std::string SigV4Signer::CanonicalQuery(const SignableRequest &_request)
{
    std::vector<std::pair<std::string, std::string>> encoded;
    encoded.reserve(_request.query.size());
    for( const auto &[k, v] : _request.query )
        encoded.emplace_back(UriEncode(k, false), UriEncode(v, false));
    std::ranges::sort(encoded);

    std::string out;
    for( const auto &[k, v] : encoded ) {
        if( !out.empty() )
            out += '&';
        out += k;
        out += '=';
        out += v;
    }
    return out;
}

std::string SigV4Signer::SignedHeaders(const SignableRequest &_request)
{
    std::string out;
    for( const auto &h : SortedCanonicalHeaders(_request) ) {
        if( !out.empty() )
            out += ';';
        out += h.first;
    }
    return out;
}

std::string SigV4Signer::CanonicalRequest(const SignableRequest &_request, std::string_view _payload_sha256_hex)
{
    std::string out;
    out += _request.method;
    out += '\n';
    out += _request.canonical_uri.empty() ? "/" : _request.canonical_uri;
    out += '\n';
    out += CanonicalQuery(_request);
    out += '\n';
    for( const auto &[name, value] : SortedCanonicalHeaders(_request) ) {
        out += name;
        out += ':';
        out += value;
        out += '\n';
    }
    out += '\n';
    out += SignedHeaders(_request);
    out += '\n';
    out += _payload_sha256_hex;
    return out;
}

std::string SigV4Signer::Authorization(const SignableRequest &_request,
                                       std::string_view _amz_date,
                                       std::string_view _payload_sha256_hex) const
{
    const std::string date(_amz_date.substr(0, 8)); // YYYYMMDD
    const std::string scope = date + "/" + m_Region + "/" + m_Service + "/aws4_request";

    const std::string canonical_request = CanonicalRequest(_request, _payload_sha256_hex);
    const std::string string_to_sign = std::string("AWS4-HMAC-SHA256\n") + std::string(_amz_date) + "\n" + scope +
                                       "\n" + ToHex(Sha256(canonical_request));

    const std::string secret = "AWS4" + m_Credentials.secret_key;
    const auto k_date = HmacSha256(std::as_bytes(std::span{secret.data(), secret.size()}), date);
    const auto k_region = HmacSha256(k_date, m_Region);
    const auto k_service = HmacSha256(k_region, m_Service);
    const auto k_signing = HmacSha256(k_service, "aws4_request");
    const std::string signature = ToHex(HmacSha256(k_signing, string_to_sign));

    return "AWS4-HMAC-SHA256 Credential=" + m_Credentials.access_key + "/" + scope +
           ", SignedHeaders=" + SignedHeaders(_request) + ", Signature=" + signature;
}

} // namespace nc::vfs::s3
