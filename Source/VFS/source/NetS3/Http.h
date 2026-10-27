// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <Base/Error.h>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace nc::vfs::s3 {

using CancelChecker = std::function<bool()>;

struct HttpRequest {
    std::string method; // GET, HEAD, PUT, POST, DELETE
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::span<const std::byte> body; // non-owning, must outlive Perform()
    CancelChecker cancel;            // optional; returning true aborts the transfer
};

struct HttpResponse {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // names are lowercased
    std::string body;

    [[nodiscard]] std::optional<std::string_view> Header(std::string_view _lowercase_name) const noexcept;
    [[nodiscard]] bool IsSuccess() const noexcept { return status >= 200 && status < 300; }
};

// Transport abstraction: the S3 client depends on this interface only, which keeps it testable.
class HttpClient
{
public:
    virtual ~HttpClient() = default;

    // Performs a blocking request. Transport-level failures are reported as errors,
    // any HTTP status (including 4xx/5xx) is reported as a successful response.
    virtual std::expected<HttpResponse, Error> Perform(const HttpRequest &_request) = 0;
};

// A thread-safe pool of reusable clients, so that keep-alive connections survive between requests.
class HttpClientPool
{
public:
    using Factory = std::function<std::unique_ptr<HttpClient>()>;

    explicit HttpClientPool(Factory _factory);

    class Lease
    {
    public:
        Lease(HttpClientPool &_pool, std::unique_ptr<HttpClient> _client) noexcept;
        Lease(Lease &&) noexcept = default;
        Lease(const Lease &) = delete;
        ~Lease();
        HttpClient *operator->() const noexcept { return m_Client.get(); }

    private:
        HttpClientPool *m_Pool;
        std::unique_ptr<HttpClient> m_Client;
    };

    Lease Acquire();

private:
    void Return(std::unique_ptr<HttpClient> _client);

    static constexpr size_t m_MaxIdle = 8;
    Factory m_Factory;
    std::mutex m_Lock;
    std::vector<std::unique_ptr<HttpClient>> m_Idle;
};

} // namespace nc::vfs::s3
