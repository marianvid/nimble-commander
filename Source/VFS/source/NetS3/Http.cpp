// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Http.h"

namespace nc::vfs::s3 {

std::optional<std::string_view> HttpResponse::Header(std::string_view _lowercase_name) const noexcept
{
    for( const auto &[name, value] : headers )
        if( name == _lowercase_name )
            return std::string_view(value);
    return std::nullopt;
}

HttpClientPool::HttpClientPool(Factory _factory) : m_Factory(std::move(_factory))
{
}

HttpClientPool::Lease HttpClientPool::Acquire()
{
    {
        const std::lock_guard lock{m_Lock};
        if( !m_Idle.empty() ) {
            auto client = std::move(m_Idle.back());
            m_Idle.pop_back();
            return Lease{*this, std::move(client)};
        }
    }
    return Lease{*this, m_Factory()};
}

void HttpClientPool::Return(std::unique_ptr<HttpClient> _client)
{
    if( !_client )
        return;
    const std::lock_guard lock{m_Lock};
    if( m_Idle.size() < m_MaxIdle )
        m_Idle.emplace_back(std::move(_client));
}

HttpClientPool::Lease::Lease(HttpClientPool &_pool, std::unique_ptr<HttpClient> _client) noexcept
    : m_Pool(&_pool), m_Client(std::move(_client))
{
}

HttpClientPool::Lease::~Lease()
{
    if( m_Client && m_Pool )
        m_Pool->Return(std::move(m_Client));
}

} // namespace nc::vfs::s3
