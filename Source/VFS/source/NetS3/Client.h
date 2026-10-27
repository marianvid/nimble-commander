// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include "Addressing.h"
#include "Http.h"
#include "SigV4.h"
#include "Xml.h"
#include <Base/Error.h>
#include <ctime>
#include <expected>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

namespace nc::vfs::s3 {

struct ClientConfig {
    Credentials credentials;
    std::string region = "us-east-1";     // default region; per-bucket regions are discovered automatically on AWS
    std::optional<Endpoint> endpoint;     // nullopt means AWS
    bool path_style = false;              // force path-style addressing
};

struct ObjectInfo {
    int64_t size = 0;
    std::time_t last_modified = -1;
};

// Converts an S3 error response into nc::Error with a meaningful POSIX code and a readable reason.
Error MakeServiceError(int _http_status, const std::optional<ServiceError> &_service_error);

// A typed facade over the S3 REST API.
// Thread-safe: every call leases its own HTTP client from the pool.
class Client
{
public:
    Client(ClientConfig _config, std::shared_ptr<HttpClientPool> _pool);

    using Page = ListObjectsPage;

    std::expected<std::vector<BucketEntry>, Error> ListBuckets(const CancelChecker &_cancel = {});

    // One page of ListObjectsV2. _delimiter is usually "/" (directory-like listing) or "" (recursive listing).
    std::expected<Page, Error> ListObjects(const std::string &_bucket,
                                           const std::string &_prefix,
                                           const std::string &_delimiter,
                                           const std::string &_continuation_token,
                                           int _max_keys = 1000,
                                           const CancelChecker &_cancel = {});

    // Iterates over all pages; _consumer returns false to stop early.
    std::expected<void, Error> ListAllObjects(const std::string &_bucket,
                                              const std::string &_prefix,
                                              const std::string &_delimiter,
                                              const std::function<bool(const Page &)> &_consumer,
                                              const CancelChecker &_cancel = {});

    std::expected<ObjectInfo, Error>
    HeadObject(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel = {});

    // Reads [_offset, _offset + _length) of an object.
    std::expected<std::string, Error> GetObjectRange(const std::string &_bucket,
                                                     const std::string &_key,
                                                     uint64_t _offset,
                                                     uint64_t _length,
                                                     const CancelChecker &_cancel = {});

    std::expected<void, Error> PutObject(const std::string &_bucket,
                                         const std::string &_key,
                                         std::span<const std::byte> _body,
                                         const CancelChecker &_cancel = {});

    std::expected<void, Error>
    DeleteObject(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel = {});

    // Server-side copy for objects up to 5 GiB.
    std::expected<void, Error> CopyObject(const std::string &_src_bucket,
                                          const std::string &_src_key,
                                          const std::string &_dst_bucket,
                                          const std::string &_dst_key,
                                          const CancelChecker &_cancel = {});

    std::expected<std::string, Error>
    CreateMultipartUpload(const std::string &_bucket, const std::string &_key, const CancelChecker &_cancel = {});

    std::expected<std::string, Error> UploadPart(const std::string &_bucket,
                                                 const std::string &_key,
                                                 const std::string &_upload_id,
                                                 int _part_number,
                                                 std::span<const std::byte> _body,
                                                 const CancelChecker &_cancel = {});

    std::expected<std::string, Error> UploadPartCopy(const std::string &_src_bucket,
                                                     const std::string &_src_key,
                                                     uint64_t _first_byte,
                                                     uint64_t _last_byte,
                                                     const std::string &_dst_bucket,
                                                     const std::string &_dst_key,
                                                     const std::string &_upload_id,
                                                     int _part_number,
                                                     const CancelChecker &_cancel = {});

    std::expected<void, Error> CompleteMultipartUpload(const std::string &_bucket,
                                                       const std::string &_key,
                                                       const std::string &_upload_id,
                                                       const std::vector<CompletedPart> &_parts,
                                                       const CancelChecker &_cancel = {});

    std::expected<void, Error>
    AbortMultipartUpload(const std::string &_bucket, const std::string &_key, const std::string &_upload_id);

    std::expected<void, Error> CreateBucket(const std::string &_bucket, const CancelChecker &_cancel = {});
    std::expected<void, Error> DeleteBucket(const std::string &_bucket, const CancelChecker &_cancel = {});

    [[nodiscard]] const ClientConfig &Config() const noexcept { return m_Config; }

private:
    struct Call {
        std::string method;
        std::string bucket;
        std::string key;
        std::vector<std::pair<std::string, std::string>> query;
        std::vector<std::pair<std::string, std::string>> headers; // extra headers, all of them get signed
        std::span<const std::byte> body;
        const CancelChecker *cancel = nullptr;
    };

    // Signs and performs a call. Non-2xx responses are converted into errors.
    // On AWS, a "wrong region" answer updates the bucket's region and the call is retried once.
    std::expected<HttpResponse, Error> Execute(const Call &_call);
    std::expected<HttpResponse, Error> ExecuteOnce(const Call &_call, const std::string &_region);

    std::string RegionForBucket(const std::string &_bucket);
    void RememberRegion(const std::string &_bucket, std::string_view _region);
    static std::string CopySource(const std::string &_bucket, const std::string &_key);

    ClientConfig m_Config;
    Addressing m_Addressing;
    std::shared_ptr<HttpClientPool> m_Pool;
    std::mutex m_RegionsLock;
    std::unordered_map<std::string, std::string> m_BucketRegions;
};

} // namespace nc::vfs::s3
