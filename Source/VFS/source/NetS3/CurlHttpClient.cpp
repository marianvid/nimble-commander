// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "CurlHttpClient.h"
#include <curl/curl.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <stdexcept>

// CURL is full of macros with C-style casts
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"

namespace nc::vfs::s3 {

namespace {

struct UploadCursor {
    std::span<const std::byte> data;
    size_t offset = 0;
};

struct TransferState {
    HttpResponse *response = nullptr;
    const CancelChecker *cancel = nullptr;
};

size_t CurlS3ReadBody(char *_buffer, size_t _size, size_t _nitems, void *_userdata)
{
    auto &cursor = *static_cast<UploadCursor *>(_userdata);
    const size_t left = cursor.data.size() - cursor.offset;
    const size_t n = std::min(left, _size * _nitems);
    if( n > 0 ) {
        std::memcpy(_buffer, cursor.data.data() + cursor.offset, n);
        cursor.offset += n;
    }
    return n;
}

int CurlS3SeekBody(void *_userdata, curl_off_t _offset, int _origin)
{
    auto &cursor = *static_cast<UploadCursor *>(_userdata);
    if( _origin != SEEK_SET || _offset < 0 || static_cast<size_t>(_offset) > cursor.data.size() )
        return CURL_SEEKFUNC_CANTSEEK;
    cursor.offset = static_cast<size_t>(_offset);
    return CURL_SEEKFUNC_OK;
}

size_t CurlS3WriteBody(char *_ptr, size_t _size, size_t _nmemb, void *_userdata)
{
    auto &state = *static_cast<TransferState *>(_userdata);
    const size_t n = _size * _nmemb;
    state.response->body.append(_ptr, n);
    return n;
}

size_t CurlS3WriteHeader(char *_ptr, size_t _size, size_t _nitems, void *_userdata)
{
    auto &state = *static_cast<TransferState *>(_userdata);
    const size_t n = _size * _nitems;
    std::string_view line(_ptr, n);
    while( !line.empty() && (line.back() == '\r' || line.back() == '\n') )
        line.remove_suffix(1);

    if( line.starts_with("HTTP/") ) {
        // A new response starts (e.g. after "100 Continue") - drop headers of the previous one.
        state.response->headers.clear();
        return n;
    }
    const auto colon = line.find(':');
    if( colon == std::string_view::npos )
        return n;

    std::string name(line.substr(0, colon));
    std::ranges::transform(name, name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string_view value = line.substr(colon + 1);
    while( !value.empty() && (value.front() == ' ' || value.front() == '\t') )
        value.remove_prefix(1);
    state.response->headers.emplace_back(std::move(name), std::string(value));
    return n;
}

int CurlS3Progress(void *_clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto &state = *static_cast<TransferState *>(_clientp);
    if( state.cancel && *state.cancel && (*state.cancel)() )
        return 1; // aborts the transfer with CURLE_ABORTED_BY_CALLBACK
    return 0;
}

Error CurlCodeToError(CURLcode _rc, const char *_details)
{
    int posix = EIO;
    switch( _rc ) {
        case CURLE_UNSUPPORTED_PROTOCOL:
            posix = EPROTONOSUPPORT;
            break;
        case CURLE_URL_MALFORMAT:
        case CURLE_BAD_FUNCTION_ARGUMENT:
            posix = EINVAL;
            break;
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            posix = EHOSTUNREACH;
            break;
        case CURLE_COULDNT_CONNECT:
            posix = ECONNREFUSED;
            break;
        case CURLE_OPERATION_TIMEDOUT:
            posix = ETIMEDOUT;
            break;
        case CURLE_ABORTED_BY_CALLBACK:
            posix = ECANCELED;
            break;
        case CURLE_SSL_CONNECT_ERROR:
        case CURLE_PEER_FAILED_VERIFICATION:
            posix = ECONNABORTED;
            break;
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
            posix = ECONNRESET;
            break;
        default:
            break;
    }
    Error err{Error::POSIX, posix};
    if( posix != ECANCELED ) {
        std::string reason = curl_easy_strerror(_rc);
        if( _details != nullptr && *_details != 0 )
            reason += std::string(": ") + _details;
        err.LocalizedFailureReason(reason);
    }
    return err;
}

} // namespace

struct CurlHttpClient::Impl {
    CURL *curl = nullptr;
};

CurlHttpClient::CurlHttpClient() : I(std::make_unique<Impl>())
{
    I->curl = curl_easy_init();
    if( I->curl == nullptr )
        throw std::runtime_error("curl_easy_init() has returned NULL");
}

CurlHttpClient::~CurlHttpClient()
{
    curl_easy_cleanup(I->curl);
}

std::expected<HttpResponse, Error> CurlHttpClient::Perform(const HttpRequest &_request)
{
    CURL *const curl = I->curl;
    curl_easy_reset(curl); // resets options but keeps live connections and caches

    HttpResponse response;
    TransferState state{&response, &_request.cancel};
    UploadCursor cursor{_request.body, 0};
    char error_buffer[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl, CURLOPT_URL, _request.url.c_str());
    curl_easy_setopt(curl, CURLOPT_PATH_AS_IS, 1L); // object keys may contain "./" or "../"
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "Nimble Commander S3");
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlS3WriteBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, CurlS3WriteHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, CurlS3Progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);

    const bool sends_body = _request.method == "PUT" || _request.method == "POST";
    if( _request.method == "HEAD" ) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    }
    else if( sends_body ) {
        // UPLOAD turns the request into a PUT, CUSTOMREQUEST then restores the intended verb.
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(curl, CURLOPT_READFUNCTION, CurlS3ReadBody);
        curl_easy_setopt(curl, CURLOPT_READDATA, &cursor);
        curl_easy_setopt(curl, CURLOPT_SEEKFUNCTION, CurlS3SeekBody);
        curl_easy_setopt(curl, CURLOPT_SEEKDATA, &cursor);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(_request.body.size()));
        if( _request.method != "PUT" )
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, _request.method.c_str());
    }
    else if( _request.method != "GET" ) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, _request.method.c_str());
    }

    struct curl_slist *headers = nullptr;
    for( const auto &[name, value] : _request.headers ) {
        const std::string line = name + ": " + value;
        headers = curl_slist_append(headers, line.c_str());
    }
    headers = curl_slist_append(headers, "Expect:");       // no "100-continue" round-trips
    headers = curl_slist_append(headers, "Content-Type:"); // the signature doesn't cover a content type
    if( sends_body )
        headers = curl_slist_append(headers, "Transfer-Encoding:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    const CURLcode rc = curl_easy_perform(curl);
    curl_slist_free_all(headers);

    if( rc != CURLE_OK )
        return std::unexpected(CurlCodeToError(rc, error_buffer));

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    response.status = static_cast<int>(status);
    return response;
}

} // namespace nc::vfs::s3

#pragma clang diagnostic pop
