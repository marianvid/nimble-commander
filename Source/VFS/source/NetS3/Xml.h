// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::vfs::s3 {

struct BucketEntry {
    std::string name;
    std::time_t creation_date = -1;
};

struct ObjectEntry {
    std::string key; // full key, already decoded
    int64_t size = 0;
    std::time_t last_modified = -1;
};

struct ListObjectsPage {
    std::vector<ObjectEntry> objects;
    std::vector<std::string> common_prefixes; // full prefixes, already decoded, ending with the delimiter
    bool is_truncated = false;
    std::string next_continuation_token;
};

struct ServiceError {
    std::string code;    // e.g. "NoSuchKey", "SignatureDoesNotMatch"
    std::string message; // human readable explanation from the server
    std::string region;  // set by some endpoints in redirect errors
};

// Parsers for S3 REST API XML documents. All of them are pure functions without any I/O.
std::optional<std::vector<BucketEntry>> ParseListAllMyBuckets(std::string_view _xml);

// Keys and prefixes are decoded when the document declares <EncodingType>url</EncodingType>.
std::optional<ListObjectsPage> ParseListObjectsV2(std::string_view _xml);

std::optional<ServiceError> ParseError(std::string_view _xml);

std::optional<std::string> ParseInitiateMultipartUpload(std::string_view _xml); // returns UploadId

// CopyObject and UploadPartCopy may answer 200 OK and still carry an <Error> body - callers check ParseError first.
std::optional<std::string> ParseCopyResultETag(std::string_view _xml);

struct CompletedPart {
    int number = 0;
    std::string etag;
};
std::string ComposeCompleteMultipartUpload(const std::vector<CompletedPart> &_parts);
std::string ComposeCreateBucketConfiguration(std::string_view _region);

} // namespace nc::vfs::s3
