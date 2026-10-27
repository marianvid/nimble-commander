// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include "Http.h"

namespace nc::vfs::s3 {

// HttpClient implementation on top of a single libcurl easy handle.
// Not thread-safe by itself - use it via HttpClientPool.
class CurlHttpClient final : public HttpClient
{
public:
    CurlHttpClient();
    CurlHttpClient(const CurlHttpClient &) = delete;
    CurlHttpClient &operator=(const CurlHttpClient &) = delete;
    ~CurlHttpClient() override;

    std::expected<HttpResponse, Error> Perform(const HttpRequest &_request) override;

private:
    struct Impl;
    std::unique_ptr<Impl> I;
};

} // namespace nc::vfs::s3
