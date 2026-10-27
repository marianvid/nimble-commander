// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Addressing.h"
#include "Encoding.h"
#include "SigV4.h"
#include <charconv>

namespace nc::vfs::s3 {

std::optional<Endpoint> ParseEndpoint(std::string_view _input)
{
    std::string_view s = TrimWhitespace(_input);
    Endpoint ep;
    if( s.starts_with("https://") ) {
        ep.https = true;
        s.remove_prefix(8);
    }
    else if( s.starts_with("http://") ) {
        ep.https = false;
        s.remove_prefix(7);
    }
    else if( s.find("://") != std::string_view::npos ) {
        return std::nullopt; // unsupported scheme
    }

    if( const auto slash = s.find('/'); slash != std::string_view::npos )
        s = s.substr(0, slash);
    if( s.empty() )
        return std::nullopt;

    if( const auto colon = s.rfind(':'); colon != std::string_view::npos && s.find(']') == std::string_view::npos ) {
        const std::string_view port = s.substr(colon + 1);
        int value = 0;
        const auto [ptr, ec] = std::from_chars(port.data(), port.data() + port.size(), value);
        if( ec != std::errc{} || ptr != port.data() + port.size() || value <= 0 || value > 65535 )
            return std::nullopt;
        ep.port = value;
        s = s.substr(0, colon);
    }
    if( s.empty() )
        return std::nullopt;
    ep.host = std::string(s);
    return ep;
}

Addressing::Addressing(std::optional<Endpoint> _custom_endpoint, bool _path_style)
    : m_Custom(std::move(_custom_endpoint)), m_PathStyle(_path_style)
{
}

bool Addressing::UsePathStyle(std::string_view _bucket) const noexcept
{
    if( _bucket.empty() )
        return true;
    if( m_PathStyle )
        return true;
    // Dotted bucket names can't be served over TLS with virtual-hosted addressing.
    return _bucket.find('.') != std::string_view::npos;
}

RequestTarget Addressing::Make(std::string_view _bucket,
                               std::string_view _key,
                               std::string_view _region,
                               const std::vector<std::pair<std::string, std::string>> &_query) const
{
    bool https = true;
    int port = -1;
    std::string host;
    if( m_Custom ) {
        https = m_Custom->https;
        port = m_Custom->port;
        host = m_Custom->host;
    }
    else {
        host = "s3." + std::string(_region) + ".amazonaws.com";
    }

    const bool path_style = UsePathStyle(_bucket);
    if( !path_style )
        host = std::string(_bucket) + "." + host;

    RequestTarget t;
    const bool default_port = port < 0 || (https && port == 443) || (!https && port == 80);
    t.host_header = default_port ? host : host + ":" + std::to_string(port);

    t.canonical_uri = "/";
    if( path_style && !_bucket.empty() ) {
        t.canonical_uri += UriEncode(_bucket, false);
        if( !_key.empty() )
            t.canonical_uri += "/";
    }
    t.canonical_uri += UriEncode(_key, true);

    SignableRequest q;
    q.query = _query;
    const std::string query = SigV4Signer::CanonicalQuery(q);

    t.url = (https ? "https://" : "http://") + t.host_header + t.canonical_uri;
    if( !query.empty() )
        t.url += "?" + query;
    return t;
}

} // namespace nc::vfs::s3
