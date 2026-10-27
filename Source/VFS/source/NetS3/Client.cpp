// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Client.h"
#include "Crypto.h"
#include "Encoding.h"
#include <cerrno>
#include <cstdlib>
#include <sys/errno.h>

namespace nc::vfs::s3 {

static int PosixCodeForServiceError(int _http_status, std::string_view _code) noexcept
{
    if( _code == "NoSuchKey" || _code == "NoSuchBucket" || _code == "NoSuchUpload" )
        return ENOENT;
    if( _code == "AccessDenied" || _code == "AllAccessDisabled" || _code == "AccountProblem" )
        return EACCES;
    if( _code == "InvalidAccessKeyId" || _code == "SignatureDoesNotMatch" || _code == "ExpiredToken" ||
        _code == "InvalidToken" || _code == "TokenRefreshRequired" )
        return EAUTH;
    if( _code == "BucketNotEmpty" )
        return ENOTEMPTY;
    if( _code == "BucketAlreadyExists" || _code == "BucketAlreadyOwnedByYou" )
        return EEXIST;
    if( _code == "EntityTooLarge" )
        return EFBIG;
    if( _code == "SlowDown" || _code == "ServiceUnavailable" || _code == "OperationAborted" )
        return EBUSY;
    if( _code == "RequestTimeTooSkewed" || _code == "RequestTimeout" )
        return ETIMEDOUT;
    if( _code == "InvalidBucketName" || _code == "KeyTooLongError" || _code == "InvalidArgument" )
        return EINVAL;
    if( _code == "NotImplemented" )
        return ENOTSUP;

    switch( _http_status ) {
        case 400:
            return EINVAL;
        case 401:
            return EAUTH;
        case 403:
            return EACCES;
        case 404:
            return ENOENT;
        case 405:
        case 501:
            return ENOTSUP;
        case 409:
            return EEXIST;
        case 411:
        case 416:
            return EINVAL;
        case 503:
            return EBUSY;
        default:
            return EIO;
    }
}

Error MakeServiceError(int _http_status, const std::optional<ServiceError> &_service_error)
{
    const std::string code = _service_error ? _service_error->code : std::string{};
    Error err{Error::POSIX, PosixCodeForServiceError(_http_status, code)};

    std::string reason = "S3: ";
    if( _service_error && !_service_error->code.empty() ) {
        reason += _service_error->code;
        if( !_service_error->message.empty() )
            reason += " - " + _service_error->message;
    }
    else {
        reason += "HTTP " + std::to_string(_http_status);
    }
    err.LocalizedFailureReason(reason);
    return err;
}

Client::Client(ClientConfig _config, std::shared_ptr<HttpClientPool> _pool)
    : m_Config(std::move(_config)), m_Addressing(m_Config.endpoint, m_Config.path_style), m_Pool(std::move(_pool))
{
    if( m_Config.region.empty() )
        m_Config.region = "us-east-1";
}

std::string Client::RegionForBucket(const std::string &_bucket)
{
    const std::lock_guard lock{m_RegionsLock};
    if( const auto it = m_BucketRegions.find(_bucket); it != m_BucketRegions.end() )
        return it->second;
    return m_Config.region;
}

void Client::RememberRegion(const std::string &_bucket, std::string_view _region)
{
    const std::lock_guard lock{m_RegionsLock};
    m_BucketRegions[_bucket] = std::string(_region);
}

std::expected<HttpResponse, Error> Client::ExecuteOnce(const Call &_call, const std::string &_region)
{
    const RequestTarget target = m_Addressing.Make(_call.bucket, _call.key, _region, _call.query);
    const std::string amz_date = AmzDateTime(std::time(nullptr));
    const std::string payload_hash =
        _call.body.empty() ? std::string(EmptyPayloadSha256) : ToHex(Sha256(_call.body));

    SignableRequest signable;
    signable.method = _call.method;
    signable.canonical_uri = target.canonical_uri;
    signable.query = _call.query;
    signable.headers.emplace_back("Host", target.host_header);
    signable.headers.emplace_back("x-amz-date", amz_date);
    signable.headers.emplace_back("x-amz-content-sha256", payload_hash);
    if( !m_Config.credentials.session_token.empty() )
        signable.headers.emplace_back("x-amz-security-token", m_Config.credentials.session_token);
    for( const auto &h : _call.headers )
        signable.headers.emplace_back(h);

    const SigV4Signer signer(m_Config.credentials, _region);
    const std::string authorization = signer.Authorization(signable, amz_date, payload_hash);

    HttpRequest request;
    request.method = _call.method;
    request.url = target.url;
    request.headers = signable.headers;
    request.headers.emplace_back("Authorization", authorization);
    request.body = _call.body;
    if( _call.cancel != nullptr )
        request.cancel = *_call.cancel;

    auto client = m_Pool->Acquire();
    return client->Perform(request);
}

std::expected<HttpResponse, Error> Client::Execute(const Call &_call)
{
    const std::string region = _call.bucket.empty() ? m_Config.region : RegionForBucket(_call.bucket);
    std::expected<HttpResponse, Error> response = ExecuteOnce(_call, region);
    if( !response )
        return response;

    if( response->IsSuccess() )
        return response;

    const std::optional<ServiceError> service_error = ParseError(response->body);

    // AWS answers requests sent to a wrong regional endpoint with the bucket's actual region.
    if( m_Addressing.IsAWS() && !_call.bucket.empty() ) {
        std::string actual;
        if( const auto header = response->Header("x-amz-bucket-region") )
            actual = std::string(*header);
        else if( service_error && !service_error->region.empty() )
            actual = service_error->region;
        if( !actual.empty() && actual != region ) {
            RememberRegion(_call.bucket, actual);
            response = ExecuteOnce(_call, actual);
            if( !response || response->IsSuccess() )
                return response;
            return std::unexpected(MakeServiceError(response->status, ParseError(response->body)));
        }
    }

    return std::unexpected(MakeServiceError(response->status, service_error));
}

std::expected<std::vector<BucketEntry>, Error> Client::ListBuckets(const CancelChecker &_cancel)
{
    Call call;
    call.method = "GET";
    call.cancel = &_cancel;
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    auto buckets = ParseListAllMyBuckets(response->body);
    if( !buckets )
        return std::unexpected(MakeServiceError(response->status, ServiceError{"MalformedResponse", "", ""}));
    return std::move(*buckets);
}

std::expected<Client::Page, Error> Client::ListObjects(const std::string &_bucket,
                                                       const std::string &_prefix,
                                                       const std::string &_delimiter,
                                                       const std::string &_continuation_token,
                                                       int _max_keys,
                                                       const CancelChecker &_cancel)
{
    Call call;
    call.method = "GET";
    call.bucket = _bucket;
    call.cancel = &_cancel;
    call.query.emplace_back("list-type", "2");
    call.query.emplace_back("encoding-type", "url");
    call.query.emplace_back("max-keys", std::to_string(_max_keys));
    call.query.emplace_back("prefix", _prefix);
    if( !_delimiter.empty() )
        call.query.emplace_back("delimiter", _delimiter);
    if( !_continuation_token.empty() )
        call.query.emplace_back("continuation-token", _continuation_token);

    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    auto page = ParseListObjectsV2(response->body);
    if( !page )
        return std::unexpected(MakeServiceError(response->status, ServiceError{"MalformedResponse", "", ""}));
    return std::move(*page);
}

std::expected<void, Error> Client::ListAllObjects(const std::string &_bucket,
                                                  const std::string &_prefix,
                                                  const std::string &_delimiter,
                                                  const std::function<bool(const Page &)> &_consumer,
                                                  const CancelChecker &_cancel)
{
    std::string token;
    while( true ) {
        if( _cancel && _cancel() )
            return std::unexpected(Error{Error::POSIX, ECANCELED});
        const auto page = ListObjects(_bucket, _prefix, _delimiter, token, 1000, _cancel);
        if( !page )
            return std::unexpected(page.error());
        if( !_consumer(*page) )
            return {};
        if( !page->is_truncated || page->next_continuation_token.empty() )
            return {};
        token = page->next_continuation_token;
    }
}

std::expected<ObjectInfo, Error>
Client::HeadObject(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel)
{
    Call call;
    call.method = "HEAD";
    call.bucket = _bucket;
    call.key = _key;
    call.cancel = &_cancel;
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());

    ObjectInfo info;
    if( const auto length = response->Header("content-length") )
        info.size = std::strtoll(std::string(*length).c_str(), nullptr, 10);
    if( const auto modified = response->Header("last-modified") )
        info.last_modified = ParseHttpDate(*modified).value_or(-1);
    return info;
}

std::expected<std::string, Error> Client::GetObjectRange(const std::string &_bucket,
                                                         const std::string &_key,
                                                         uint64_t _offset,
                                                         uint64_t _length,
                                                         const CancelChecker &_cancel)
{
    if( _length == 0 )
        return std::string{};

    Call call;
    call.method = "GET";
    call.bucket = _bucket;
    call.key = _key;
    call.cancel = &_cancel;
    call.headers.emplace_back("Range",
                              "bytes=" + std::to_string(_offset) + "-" + std::to_string(_offset + _length - 1));
    auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());

    std::string body = std::move(response->body);
    if( response->status == 200 ) {
        // The server ignored the Range header and sent the whole object.
        body = _offset < body.size() ? body.substr(_offset, _length) : std::string{};
    }
    return body;
}

std::expected<void, Error> Client::PutObject(const std::string &_bucket,
                                             const std::string &_key,
                                             std::span<const std::byte> _body,
                                             const CancelChecker &_cancel)
{
    Call call;
    call.method = "PUT";
    call.bucket = _bucket;
    call.key = _key;
    call.body = _body;
    call.cancel = &_cancel;
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return {};
}

std::expected<void, Error>
Client::DeleteObject(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel)
{
    Call call;
    call.method = "DELETE";
    call.bucket = _bucket;
    call.key = _key;
    call.cancel = &_cancel;
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return {};
}

std::string Client::CopySource(const std::string &_bucket, const std::string &_key)
{
    return "/" + UriEncode(_bucket, false) + "/" + UriEncode(_key, true);
}

// Some S3 operations report failures inside a "200 OK" response body.
static std::expected<void, Error> CheckEmbeddedError(const HttpResponse &_response)
{
    if( const auto err = ParseError(_response.body) )
        return std::unexpected(MakeServiceError(_response.status, err));
    return {};
}

std::expected<void, Error> Client::CopyObject(const std::string &_src_bucket,
                                              const std::string &_src_key,
                                              const std::string &_dst_bucket,
                                              const std::string &_dst_key,
                                              const CancelChecker &_cancel)
{
    Call call;
    call.method = "PUT";
    call.bucket = _dst_bucket;
    call.key = _dst_key;
    call.cancel = &_cancel;
    call.headers.emplace_back("x-amz-copy-source", CopySource(_src_bucket, _src_key));
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return CheckEmbeddedError(*response);
}

std::expected<std::string, Error>
Client::CreateMultipartUpload(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel)
{
    Call call;
    call.method = "POST";
    call.bucket = _bucket;
    call.key = _key;
    call.cancel = &_cancel;
    call.query.emplace_back("uploads", "");
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    auto id = ParseInitiateMultipartUpload(response->body);
    if( !id )
        return std::unexpected(MakeServiceError(response->status, ServiceError{"MalformedResponse", "", ""}));
    return std::move(*id);
}

std::expected<std::string, Error> Client::UploadPart(const std::string &_bucket,
                                                     const std::string &_key,
                                                     const std::string &_upload_id,
                                                     int _part_number,
                                                     std::span<const std::byte> _body,
                                                     const CancelChecker &_cancel)
{
    Call call;
    call.method = "PUT";
    call.bucket = _bucket;
    call.key = _key;
    call.body = _body;
    call.cancel = &_cancel;
    call.query.emplace_back("partNumber", std::to_string(_part_number));
    call.query.emplace_back("uploadId", _upload_id);
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    const auto etag = response->Header("etag");
    if( !etag )
        return std::unexpected(MakeServiceError(response->status, ServiceError{"MissingETag", "", ""}));
    return std::string(*etag);
}

std::expected<std::string, Error> Client::UploadPartCopy(const std::string &_src_bucket,
                                                         const std::string &_src_key,
                                                         uint64_t _first_byte,
                                                         uint64_t _last_byte,
                                                         const std::string &_dst_bucket,
                                                         const std::string &_dst_key,
                                                         const std::string &_upload_id,
                                                         int _part_number,
                                                         const CancelChecker &_cancel)
{
    Call call;
    call.method = "PUT";
    call.bucket = _dst_bucket;
    call.key = _dst_key;
    call.cancel = &_cancel;
    call.query.emplace_back("partNumber", std::to_string(_part_number));
    call.query.emplace_back("uploadId", _upload_id);
    call.headers.emplace_back("x-amz-copy-source", CopySource(_src_bucket, _src_key));
    call.headers.emplace_back("x-amz-copy-source-range",
                              "bytes=" + std::to_string(_first_byte) + "-" + std::to_string(_last_byte));
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    if( const auto embedded = CheckEmbeddedError(*response); !embedded )
        return std::unexpected(embedded.error());
    auto etag = ParseCopyResultETag(response->body);
    if( !etag )
        return std::unexpected(MakeServiceError(response->status, ServiceError{"MissingETag", "", ""}));
    return std::move(*etag);
}

std::expected<void, Error> Client::CompleteMultipartUpload(const std::string &_bucket,
                                                           const std::string &_key,
                                                           const std::string &_upload_id,
                                                           const std::vector<CompletedPart> &_parts,
                                                           const CancelChecker &_cancel)
{
    const std::string xml = ComposeCompleteMultipartUpload(_parts);
    Call call;
    call.method = "POST";
    call.bucket = _bucket;
    call.key = _key;
    call.cancel = &_cancel;
    call.body = std::as_bytes(std::span{xml.data(), xml.size()});
    call.query.emplace_back("uploadId", _upload_id);
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return CheckEmbeddedError(*response);
}

std::expected<void, Error>
Client::AbortMultipartUpload(const std::string &_bucket, const std::string &_key, const std::string &_upload_id)
{
    Call call;
    call.method = "DELETE";
    call.bucket = _bucket;
    call.key = _key;
    call.query.emplace_back("uploadId", _upload_id);
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return {};
}

std::expected<void, Error> Client::CreateBucket(const std::string &_bucket, const CancelChecker &_cancel)
{
    std::string xml;
    if( m_Config.region != "us-east-1" )
        xml = ComposeCreateBucketConfiguration(m_Config.region);
    Call call;
    call.method = "PUT";
    call.bucket = _bucket;
    call.cancel = &_cancel;
    call.body = std::as_bytes(std::span{xml.data(), xml.size()});
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    RememberRegion(_bucket, m_Config.region);
    return {};
}

std::expected<void, Error> Client::DeleteBucket(const std::string &_bucket, const CancelChecker &_cancel)
{
    Call call;
    call.method = "DELETE";
    call.bucket = _bucket;
    call.cancel = &_cancel;
    const auto response = Execute(call);
    if( !response )
        return std::unexpected(response.error());
    return {};
}

} // namespace nc::vfs::s3
