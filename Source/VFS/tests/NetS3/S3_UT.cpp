// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "../Tests.h"
#include <VFS/NetS3.h>
#include "../../source/NetS3/Addressing.h"
#include "../../source/NetS3/Client.h"
#include "../../source/NetS3/Crypto.h"
#include "../../source/NetS3/Encoding.h"
#include "../../source/NetS3/File.h"
#include "../../source/NetS3/PathMapper.h"
#include "../../source/NetS3/SigV4.h"
#include "../../source/NetS3/Xml.h"
#include <VFS/VFSListing.h>
#include <map>
#include <set>

#define PREFIX "nc::vfs::s3 "

namespace S3Tests {

using namespace nc;
using namespace nc::vfs;
using namespace nc::vfs::s3;

[[clang::no_destroy]] static const Credentials g_ExampleCredentials{"AKIAIOSFODNN7EXAMPLE", "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY", ""};

//==================================================================================================
// Pure building blocks
//==================================================================================================

TEST_CASE(PREFIX "SigV4 matches the AWS documentation examples")
{
    const SigV4Signer signer(g_ExampleCredentials, "us-east-1");
    SECTION("GET Object with Range")
    {
        SignableRequest r;
        r.method = "GET";
        r.canonical_uri = "/test.txt";
        r.headers = {{"Host", "examplebucket.s3.amazonaws.com"},
                     {"Range", "bytes=0-9"},
                     {"x-amz-content-sha256", std::string(EmptyPayloadSha256)},
                     {"x-amz-date", "20130524T000000Z"}};
        CHECK(signer.Authorization(r, "20130524T000000Z", EmptyPayloadSha256) ==
              "AWS4-HMAC-SHA256 Credential=AKIAIOSFODNN7EXAMPLE/20130524/us-east-1/s3/aws4_request, "
              "SignedHeaders=host;range;x-amz-content-sha256;x-amz-date, "
              "Signature=f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    }
    SECTION("GET Bucket (List Objects)")
    {
        SignableRequest r;
        r.method = "GET";
        r.canonical_uri = "/";
        r.query = {{"prefix", "J"}, {"max-keys", "2"}};
        r.headers = {{"Host", "examplebucket.s3.amazonaws.com"},
                     {"x-amz-content-sha256", std::string(EmptyPayloadSha256)},
                     {"x-amz-date", "20130524T000000Z"}};
        CHECK(signer.Authorization(r, "20130524T000000Z", EmptyPayloadSha256).ends_with(
            "Signature=34b48302e7b5fa45bde8084f4b7868a86f0a534bc59db6670ed5711ef69dc6f7"));
    }
}

TEST_CASE(PREFIX "Encoding helpers")
{
    CHECK(UriEncode("a b/ç+x~.txt", true) == "a%20b/%C3%A7%2Bx~.txt");
    CHECK(UriEncode("a/b", false) == "a%2Fb");
    CHECK(FormUrlDecode("a+b%2Bc%2F") == "a b+c/");
    CHECK(ParseIso8601("2009-10-12T17:50:30.000Z") == 1255369830);
    CHECK(ParseHttpDate("Mon, 12 Oct 2009 17:50:30 GMT") == 1255369830);
    CHECK(AmzDateTime(1255369830) == "20091012T175030Z");
    CHECK(!ParseIso8601("garbage"));
}

TEST_CASE(PREFIX "XML parsing")
{
    const auto page = ParseListObjectsV2(
        R"(<?xml version="1.0" encoding="UTF-8"?><ListBucketResult xmlns="http://s3.amazonaws.com/doc/2006-03-01/">)"
        R"(<EncodingType>url</EncodingType><IsTruncated>true</IsTruncated><NextContinuationToken>t==</NextContinuationToken>)"
        R"(<Contents><Key>d%2Fa+b.txt</Key><LastModified>2024-01-02T03:04:05.000Z</LastModified><Size>12345678901</Size></Contents>)"
        R"(<CommonPrefixes><Prefix>d%2Fsub%2F</Prefix></CommonPrefixes></ListBucketResult>)");
    REQUIRE(page);
    REQUIRE(page->objects.size() == 1);
    CHECK(page->objects[0].key == "d/a b.txt");
    CHECK(page->objects[0].size == 12345678901LL);
    CHECK(page->common_prefixes == std::vector<std::string>{"d/sub/"});
    CHECK(page->is_truncated);
    CHECK(page->next_continuation_token == "t==");

    const auto err = ParseError("<Error><Code>NoSuchKey</Code><Message>gone</Message></Error>");
    REQUIRE(err);
    CHECK(err->code == "NoSuchKey");
    CHECK(MakeServiceError(404, err) == Error{Error::POSIX, ENOENT});
    CHECK(MakeServiceError(403, ServiceError{"SignatureDoesNotMatch", "", ""}) == Error{Error::POSIX, EAUTH});
    CHECK(MakeServiceError(500, std::nullopt) == Error{Error::POSIX, EIO});
}

TEST_CASE(PREFIX "Addressing")
{
    const Addressing aws(std::nullopt, false);
    auto t = aws.Make("bucket", "a b/c.txt", "eu-central-1", {});
    CHECK(t.url == "https://bucket.s3.eu-central-1.amazonaws.com/a%20b/c.txt");
    CHECK(t.host_header == "bucket.s3.eu-central-1.amazonaws.com");
    CHECK(t.canonical_uri == "/a%20b/c.txt");

    t = aws.Make("dotted.bucket", "k", "us-east-1", {{"prefix", "a/"}, {"list-type", "2"}});
    CHECK(t.url == "https://s3.us-east-1.amazonaws.com/dotted.bucket/k?list-type=2&prefix=a%2F");

    const auto ep = ParseEndpoint("http://minio.lan:9000/");
    REQUIRE(ep);
    CHECK(ep->https == false);
    CHECK(ep->port == 9000);
    const Addressing minio(ep, true);
    t = minio.Make("bkt", "x/y", "us-east-1", {{"uploads", ""}});
    CHECK(t.url == "http://minio.lan:9000/bkt/x/y?uploads=");
    CHECK(t.host_header == "minio.lan:9000");

    CHECK(!ParseEndpoint("ftp://x"));
    CHECK(!ParseEndpoint("http://h:99999"));
    CHECK(ParseEndpoint("r2.example.com")->https);
}

TEST_CASE(PREFIX "PathMapper")
{
    const PathMapper any("");
    CHECK(any.Map("/")->IsServiceRoot());
    CHECK(any.Map("/b")->IsBucketRoot());
    CHECK(*any.Map("/b/x/y/") == Location{"b", "x/y"});
    CHECK(!any.Map("relative"));
    CHECK(!any.Map("/b//x"));
    CHECK(!any.Map("/b/../x"));
    CHECK(any.ToVFSDirectory({"b", "x"}) == "/b/x/");

    const PathMapper fixed("fb");
    CHECK(fixed.Map("/")->IsBucketRoot());
    CHECK(*fixed.Map("/x/y") == Location{"fb", "x/y"});
    CHECK(fixed.ToVFSDirectory({"fb", ""}) == "/");
}

TEST_CASE(PREFIX "Multipart part sizes")
{
    constexpr uint64_t mib = 1024 * 1024;
    CHECK(s3::File::PartSizeFor(1) == 16 * mib);
    CHECK(s3::File::PartSizeFor(100 * mib) == 16 * mib);
    const uint64_t huge = 4ull * 1024 * 1024 * mib; // 4 TiB
    const uint64_t part = s3::File::PartSizeFor(huge);
    CHECK(part % mib == 0);
    CHECK((huge + part - 1) / part <= 10'000);
}

TEST_CASE(PREFIX "AWS shared profiles")
{
    const std::string_view credentials = "# comment\n"
                                         "[default]\n"
                                         "aws_access_key_id = AKDEF\n"
                                         "aws_secret_access_key = SECDEF\n"
                                         "\n"
                                         "[work]\n"
                                         "aws_access_key_id=AKWORK\n"
                                         "aws_secret_access_key=SECWORK\n"
                                         "aws_session_token=TOKEN\n";
    const std::string_view config = "[default]\n"
                                    "region = eu-central-1\n"
                                    "[profile work]\n"
                                    "region = us-west-2\n"
                                    "[profile r2]\n"
                                    "aws_access_key_id = AKR2\n"
                                    "aws_secret_access_key = SECR2\n"
                                    "endpoint_url = https://acc.r2.cloudflarestorage.com\n"
                                    "[sso-session corp]\n"
                                    "region = ignored\n";
    const auto profiles = ParseAwsProfiles(credentials, config);
    REQUIRE(profiles.size() == 3);
    CHECK(profiles[0] == AwsProfile{"default", "AKDEF", "SECDEF", "", "eu-central-1", ""});
    CHECK(profiles[1] == AwsProfile{"r2", "AKR2", "SECR2", "", "", "https://acc.r2.cloudflarestorage.com"});
    CHECK(profiles[2] == AwsProfile{"work", "AKWORK", "SECWORK", "TOKEN", "us-west-2", ""});
}

//==================================================================================================
// An in-memory S3 server that also validates request signatures
//==================================================================================================

class FakeS3 final : public HttpClient
{
public:
    explicit FakeS3(Credentials _credentials) : m_Credentials(std::move(_credentials)) {}

    std::map<std::string, std::map<std::string, std::string>> buckets;
    std::vector<std::string> log; // "METHOD path?query" of every request
    int max_keys_cap = 1000;      // to exercise pagination

    std::expected<HttpResponse, Error> Perform(const HttpRequest &_request) override
    {
        const auto [path, query] = SplitUrl(_request.url);
        log.push_back(_request.method + " " + path + (query.empty() ? "" : "?" + query));
        if( !VerifySignature(_request, path, query) )
            return Respond(403, R"(<Error><Code>SignatureDoesNotMatch</Code></Error>)");

        const auto params = ParseQuery(query);
        const auto [bucket, key] = SplitPath(path);
        const auto header = [&](std::string_view _name) -> std::string {
            for( const auto &[n, v] : _request.headers )
                if( LowerAscii(n) == _name )
                    return v;
            return {};
        };
        const std::string body(reinterpret_cast<const char *>(_request.body.data()), _request.body.size());

        if( bucket.empty() ) {
            std::string xml = "<ListAllMyBucketsResult><Buckets>";
            for( const auto &b : buckets )
                xml += "<Bucket><Name>" + b.first + "</Name><CreationDate>2020-01-01T00:00:00.000Z</CreationDate></Bucket>";
            return Respond(200, xml + "</Buckets></ListAllMyBucketsResult>");
        }
        if( key.empty() ) {
            if( _request.method == "PUT" ) {
                buckets[bucket];
                return Respond(200, "");
            }
            if( !buckets.contains(bucket) )
                return Respond(404, "<Error><Code>NoSuchBucket</Code></Error>");
            if( _request.method == "DELETE" ) {
                if( !buckets[bucket].empty() )
                    return Respond(409, "<Error><Code>BucketNotEmpty</Code></Error>");
                buckets.erase(bucket);
                return Respond(204, "");
            }
            return List(buckets[bucket], params);
        }
        if( !buckets.contains(bucket) )
            return Respond(404, "<Error><Code>NoSuchBucket</Code></Error>");
        auto &objects = buckets[bucket];

        if( _request.method == "HEAD" ) {
            const auto it = objects.find(key);
            if( it == objects.end() )
                return Respond(404, "");
            auto r = Respond(200, "");
            r.headers.emplace_back("content-length", std::to_string(it->second.size()));
            r.headers.emplace_back("last-modified", "Mon, 12 Oct 2009 17:50:30 GMT");
            return r;
        }
        if( _request.method == "GET" ) {
            const auto it = objects.find(key);
            if( it == objects.end() )
                return Respond(404, "<Error><Code>NoSuchKey</Code></Error>");
            const std::string range = header("range");
            uint64_t first = 0;
            uint64_t last = 0;
            if( std::sscanf(range.c_str(), "bytes=%llu-%llu", &first, &last) == 2 )
                return Respond(206, it->second.substr(first, last - first + 1));
            return Respond(200, it->second);
        }
        if( _request.method == "DELETE" ) {
            if( params.contains("uploadId") ) {
                m_Uploads.erase(params.at("uploadId"));
                return Respond(204, "");
            }
            objects.erase(key);
            return Respond(204, "");
        }
        if( _request.method == "POST" && params.contains("uploads") ) {
            const std::string id = "upload" + std::to_string(++m_LastUpload);
            m_Uploads[id];
            return Respond(200,
                           "<InitiateMultipartUploadResult><UploadId>" + id +
                               "</UploadId></InitiateMultipartUploadResult>");
        }
        if( _request.method == "POST" && params.contains("uploadId") ) {
            auto &parts = m_Uploads[params.at("uploadId")];
            std::string assembled;
            for( const auto &p : parts )
                assembled += p.second;
            objects[key] = assembled;
            completed_multipart_uploads++;
            return Respond(200, "<CompleteMultipartUploadResult/>");
        }
        if( _request.method == "PUT" ) {
            std::string data = body;
            if( const auto source = header("x-amz-copy-source"); !source.empty() ) {
                const auto [src_bucket, src_key] = SplitPath(source);
                const auto &src = buckets[src_bucket];
                const auto it = src.find(src_key);
                if( it == src.end() )
                    return Respond(404, "<Error><Code>NoSuchKey</Code></Error>");
                data = it->second;
                if( const auto range = header("x-amz-copy-source-range"); !range.empty() ) {
                    uint64_t first = 0;
                    uint64_t last = 0;
                    std::sscanf(range.c_str(), "bytes=%llu-%llu", &first, &last);
                    data = data.substr(first, last - first + 1);
                }
            }
            if( params.contains("uploadId") ) {
                m_Uploads[params.at("uploadId")][std::stoi(params.at("partNumber"))] = data;
                auto r = Respond(200, header("x-amz-copy-source").empty() ? ""
                                                                           : "<CopyPartResult><ETag>\"e\"</ETag></CopyPartResult>");
                r.headers.emplace_back("etag", "\"e\"");
                return r;
            }
            objects[key] = data;
            return Respond(200, header("x-amz-copy-source").empty() ? "" : "<CopyObjectResult><ETag>\"e\"</ETag></CopyObjectResult>");
        }
        return Respond(405, "");
    }

    int completed_multipart_uploads = 0;

private:
    static std::string LowerAscii(std::string _s)
    {
        for( auto &c : _s )
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return _s;
    }

    static HttpResponse Respond(int _status, std::string _body)
    {
        HttpResponse r;
        r.status = _status;
        r.body = std::move(_body);
        return r;
    }

    static std::pair<std::string, std::string> SplitUrl(const std::string &_url)
    {
        const auto scheme_end = _url.find("://");
        const auto path_begin = _url.find('/', scheme_end + 3);
        const std::string rest = path_begin == std::string::npos ? "/" : _url.substr(path_begin);
        const auto q = rest.find('?');
        if( q == std::string::npos )
            return {rest, ""};
        return {rest.substr(0, q), rest.substr(q + 1)};
    }

    static std::map<std::string, std::string> ParseQuery(const std::string &_query)
    {
        std::map<std::string, std::string> params;
        size_t pos = 0;
        while( pos < _query.size() ) {
            auto amp = _query.find('&', pos);
            if( amp == std::string::npos )
                amp = _query.size();
            const std::string item = _query.substr(pos, amp - pos);
            const auto eq = item.find('=');
            params[FormUrlDecode(item.substr(0, eq))] =
                eq == std::string::npos ? "" : FormUrlDecode(item.substr(eq + 1));
            pos = amp + 1;
        }
        return params;
    }

    // "/bucket/a%20b/c" -> {"bucket", "a b/c"}
    static std::pair<std::string, std::string> SplitPath(const std::string &_path)
    {
        std::string p = FormUrlDecode(_path);
        if( p.starts_with('/') )
            p.erase(0, 1);
        const auto slash = p.find('/');
        if( slash == std::string::npos )
            return {p, ""};
        return {p.substr(0, slash), p.substr(slash + 1)};
    }

    bool VerifySignature(const HttpRequest &_request, const std::string &_path, const std::string &_query) const
    {
        std::string authorization;
        std::string amz_date;
        std::string payload;
        SignableRequest r;
        r.method = _request.method;
        r.canonical_uri = _path;
        for( const auto &[k, v] : ParseQuery(_query) )
            r.query.emplace_back(k, v);
        for( const auto &[n, v] : _request.headers ) {
            const auto name = LowerAscii(n);
            if( name == "authorization" ) {
                authorization = v;
                continue;
            }
            if( name == "x-amz-date" )
                amz_date = v;
            if( name == "x-amz-content-sha256" )
                payload = v;
            r.headers.emplace_back(n, v);
        }
        const SigV4Signer signer(m_Credentials, "us-east-1");
        return authorization == signer.Authorization(r, amz_date, payload);
    }

    HttpResponse List(const std::map<std::string, std::string> &_objects,
                      const std::map<std::string, std::string> &_params) const
    {
        const std::string prefix = _params.contains("prefix") ? _params.at("prefix") : "";
        const std::string delimiter = _params.contains("delimiter") ? _params.at("delimiter") : "";
        const int max_keys = std::min(max_keys_cap, _params.contains("max-keys") ? std::stoi(_params.at("max-keys")) : 1000);
        const std::string after = _params.contains("continuation-token") ? _params.at("continuation-token") : "";

        // Flatten into an ordered sequence of objects and common prefixes, as S3 does.
        std::vector<std::pair<std::string, bool>> items; // name, is_prefix
        std::set<std::string> seen_prefixes;
        for( const auto &[key, data] : _objects ) {
            if( !key.starts_with(prefix) )
                continue;
            const auto rest = key.substr(prefix.size());
            if( const auto d = delimiter.empty() ? std::string::npos : rest.find(delimiter); d != std::string::npos ) {
                const auto cp = prefix + rest.substr(0, d + 1);
                if( seen_prefixes.insert(cp).second )
                    items.emplace_back(cp, true);
            }
            else {
                items.emplace_back(key, false);
            }
        }

        std::string xml = "<ListBucketResult>";
        int count = 0;
        bool truncated = false;
        std::string last;
        for( const auto &[name, is_prefix] : items ) {
            if( !after.empty() && name <= after )
                continue;
            if( count == max_keys ) {
                truncated = true;
                break;
            }
            if( is_prefix )
                xml += "<CommonPrefixes><Prefix>" + name + "</Prefix></CommonPrefixes>";
            else
                xml += "<Contents><Key>" + name + "</Key><LastModified>2024-01-02T03:04:05.000Z</LastModified><Size>" +
                       std::to_string(_objects.at(name).size()) + "</Size></Contents>";
            last = name;
            ++count;
        }
        xml += std::string("<IsTruncated>") + (truncated ? "true" : "false") + "</IsTruncated>";
        if( truncated )
            xml += "<NextContinuationToken>" + last + "</NextContinuationToken>";
        return Respond(200, xml + "</ListBucketResult>");
    }

    Credentials m_Credentials;
    std::map<std::string, std::map<int, std::string>> m_Uploads;
    int m_LastUpload = 0;
};

struct FakeEnv {
    std::shared_ptr<FakeS3> server = std::make_shared<FakeS3>(g_ExampleCredentials);
    std::shared_ptr<HttpClientPool> pool;

    FakeEnv()
    {
        // The pool hands out non-owning wrappers around the single shared fake server.
        struct Proxy final : HttpClient {
            std::shared_ptr<FakeS3> target;
            std::expected<HttpResponse, Error> Perform(const HttpRequest &_r) override { return target->Perform(_r); }
        };
        pool = std::make_shared<HttpClientPool>([s = server] {
            auto p = std::make_unique<Proxy>();
            p->target = s;
            return p;
        });
    }

    std::shared_ptr<S3Host> MakeHost(std::string _bucket = "")
    {
        HostConfiguration config;
        config.endpoint = "http://fake";
        config.path_style = true;
        config.bucket = std::move(_bucket);
        config.access_key = g_ExampleCredentials.access_key;
        config.secret_key = g_ExampleCredentials.secret_key;
        return std::make_shared<S3Host>(config, pool);
    }
};

static std::vector<std::string> Names(const VFSListing &_listing)
{
    std::vector<std::string> names;
    for( unsigned i = 0; i < _listing.Count(); ++i )
        names.push_back(_listing.Filename(i) + (_listing.IsDir(i) && !_listing.IsDotDot(i) ? "/" : ""));
    return names;
}

static std::string ReadAll(const std::shared_ptr<S3Host> &_host, const std::string &_path)
{
    auto file = _host->CreateFile(_path, {});
    REQUIRE(file);
    REQUIRE((*file)->Open(VFSFlags::OF_Read, {}));
    auto data = (*file)->ReadFile();
    REQUIRE(data);
    return {data->begin(), data->end()};
}

static void WriteAll(const std::shared_ptr<S3Host> &_host, const std::string &_path, const std::string &_data)
{
    auto file = _host->CreateFile(_path, {});
    REQUIRE(file);
    REQUIRE((*file)->Open(VFSFlags::OF_Write | VFSFlags::OF_Create, {}));
    REQUIRE((*file)->SetUploadSize(_data.size()));
    // Feed the data in uneven chunks, like the copying machinery does.
    size_t pos = 0;
    while( pos < _data.size() ) {
        const size_t n = std::min<size_t>(_data.size() - pos, 3'000'017);
        const auto rc = (*file)->Write(_data.data() + pos, n);
        REQUIRE(rc);
        pos += *rc;
    }
    REQUIRE((*file)->Close());
}

TEST_CASE(PREFIX "Host: bucket listing and directory view")
{
    FakeEnv env;
    env.server->buckets["photos"] = {{"a.txt", "hello"}, {"dir/", ""}, {"dir/b.txt", "bb"}, {"dir/sub/c.txt", "ccc"}};
    env.server->buckets["docs"] = {};
    const auto host = env.MakeHost();

    auto root = host->FetchDirectoryListing("/", 0, {});
    REQUIRE(root);
    CHECK(Names(**root) == std::vector<std::string>{"docs/", "photos/"});

    auto photos = host->FetchDirectoryListing("/photos", 0, {});
    REQUIRE(photos);
    CHECK(Names(**photos) == std::vector<std::string>{"..", "a.txt", "dir/"});

    auto dir = host->FetchDirectoryListing("/photos/dir/", 0, {});
    REQUIRE(dir);
    CHECK(Names(**dir) == std::vector<std::string>{"..", "b.txt", "sub/"});

    const auto st = host->Stat("/photos/dir/sub/c.txt", 0, {}); // not cached - HEAD
    REQUIRE(st);
    CHECK(st->size == 3);
    CHECK(host->Stat("/photos/dir/sub", 0, {})->mode_bits.dir);
    CHECK(host->Stat("/photos/nope", 0, {}).error() == Error{Error::POSIX, ENOENT});
}

TEST_CASE(PREFIX "Host: pagination is followed")
{
    FakeEnv env;
    auto &objects = env.server->buckets["b"];
    for( int i = 0; i < 25; ++i )
        objects["k" + std::to_string(100 + i)] = "x";
    env.server->max_keys_cap = 10;
    const auto host = env.MakeHost("b");
    auto listing = host->FetchDirectoryListing("/", 0, {});
    REQUIRE(listing);
    CHECK((*listing)->Count() == 25);
}

TEST_CASE(PREFIX "Host: write, read, mkdir, rename, delete")
{
    FakeEnv env;
    env.server->buckets["b"] = {};
    const auto host = env.MakeHost("b");

    SECTION("small file round-trip")
    {
        WriteAll(host, "/hello.txt", "Hello, S3!");
        CHECK(env.server->buckets["b"]["hello.txt"] == "Hello, S3!");
        CHECK(ReadAll(host, "/hello.txt") == "Hello, S3!");
        CHECK(host->Stat("/hello.txt", 0, {})->size == 10);
    }
    SECTION("zero-byte file")
    {
        WriteAll(host, "/empty", "");
        CHECK(env.server->buckets["b"].contains("empty"));
        CHECK(ReadAll(host, "/empty").empty());
    }
    SECTION("multipart upload and ranged reads")
    {
        std::string big(40 * 1024 * 1024 + 123, '\0');
        for( size_t i = 0; i < big.size(); ++i )
            big[i] = static_cast<char>(i * 7 % 251);
        WriteAll(host, "/big.bin", big);
        CHECK(env.server->completed_multipart_uploads == 1);
        CHECK(env.server->buckets["b"]["big.bin"] == big);
        CHECK(ReadAll(host, "/big.bin") == big);

        auto file = host->CreateFile("/big.bin", {});
        REQUIRE((*file)->Open(VFSFlags::OF_Read, {}));
        char buf[16];
        REQUIRE((*file)->ReadAt(30'000'000, buf, sizeof(buf)) == sizeof(buf));
        CHECK(std::string_view(buf, sizeof(buf)) == std::string_view(big).substr(30'000'000, sizeof(buf)));
    }
    SECTION("directories")
    {
        REQUIRE(host->CreateDirectory("/folder", 0755, {}));
        CHECK(env.server->buckets["b"].contains("folder/"));
        WriteAll(host, "/folder/x.txt", "x");
        CHECK(Names(**host->FetchDirectoryListing("/folder", 0, {})) == std::vector<std::string>{"..", "x.txt"});
        CHECK(host->RemoveDirectory("/folder", {}).error() == Error{Error::POSIX, ENOTEMPTY});
        REQUIRE(host->Unlink("/folder/x.txt", {}));
        REQUIRE(host->RemoveDirectory("/folder", {}));
        CHECK(env.server->buckets["b"].empty());
    }
    SECTION("rename a file and a directory")
    {
        WriteAll(host, "/a.txt", "A");
        REQUIRE(host->Rename("/a.txt", "/b.txt", {}));
        CHECK(!env.server->buckets["b"].contains("a.txt"));
        CHECK(env.server->buckets["b"]["b.txt"] == "A");

        env.server->buckets["b"]["d1/one"] = "1";
        env.server->buckets["b"]["d1/deep/two"] = "2";
        REQUIRE(host->Rename("/d1", "/d2", {}));
        CHECK(env.server->buckets["b"]["d2/one"] == "1");
        CHECK(env.server->buckets["b"]["d2/deep/two"] == "2");
        CHECK(!env.server->buckets["b"].contains("d1/one"));
        CHECK(host->Rename("/d2", "/d2/inner", {}).error() == Error{Error::POSIX, EINVAL});
    }
    SECTION("keys with special characters")
    {
        WriteAll(host, "/spaced name+plus (1)/ü.txt", "u");
        CHECK(env.server->buckets["b"]["spaced name+plus (1)/ü.txt"] == "u");
        auto listing = host->FetchDirectoryListing("/spaced name+plus (1)", VFSFlags::F_ForceRefresh, {});
        REQUIRE(listing);
        CHECK(Names(**listing) == std::vector<std::string>{"..", "ü.txt"});
    }
    SECTION("open-for-write semantics")
    {
        WriteAll(host, "/exists", "1");
        auto file = host->CreateFile("/exists", {});
        CHECK((*file)->Open(VFSFlags::OF_Write | VFSFlags::OF_Create | VFSFlags::OF_NoExist, {}).error() ==
              Error{Error::POSIX, EEXIST});
        auto missing = host->CreateFile("/missing", {});
        CHECK((*missing)->Open(VFSFlags::OF_Write, {}).error() == Error{Error::POSIX, ENOENT});
    }
}

TEST_CASE(PREFIX "Host: wrong credentials are reported on connect")
{
    FakeEnv env;
    HostConfiguration config;
    config.endpoint = "http://fake";
    config.path_style = true;
    config.access_key = g_ExampleCredentials.access_key;
    config.secret_key = "wrong";
    try {
        std::ignore = std::make_shared<S3Host>(config, env.pool);
        FAIL("expected an exception");
    } catch( const ErrorException &e ) {
        CHECK(e.error() == Error{Error::POSIX, EAUTH});
        CHECK(e.error().LocalizedFailureReason().find("SignatureDoesNotMatch") != std::string::npos);
    }
}

//==================================================================================================
// Integration test against a real S3-compatible server, e.g. a local MinIO:
//   NC_S3_IT_ENDPOINT=http://127.0.0.1:9010 NC_S3_IT_ACCESS_KEY=... NC_S3_IT_SECRET_KEY=... VFSUT "nc::vfs::s3*"
// Skipped when the environment variables are not set.
//==================================================================================================

TEST_CASE(PREFIX "Integration: real server round-trip")
{
    const char *endpoint = std::getenv("NC_S3_IT_ENDPOINT");
    const char *access_key = std::getenv("NC_S3_IT_ACCESS_KEY");
    const char *secret_key = std::getenv("NC_S3_IT_SECRET_KEY");
    if( endpoint == nullptr || access_key == nullptr || secret_key == nullptr )
        return;

    HostConfiguration config;
    config.endpoint = endpoint;
    config.path_style = true;
    config.access_key = access_key;
    config.secret_key = secret_key;
    const auto host = std::make_shared<S3Host>(config);

    const std::string bucket = "/nc-it-" + std::to_string(std::time(nullptr));
    REQUIRE(host->CreateDirectory(bucket, 0755, {}));

    WriteAll(host, bucket + "/small.txt", "small");
    CHECK(ReadAll(host, bucket + "/small.txt") == "small");

    std::string big(21 * 1024 * 1024 + 7, '\0');
    for( size_t i = 0; i < big.size(); ++i )
        big[i] = static_cast<char>(i * 13 % 247);
    WriteAll(host, bucket + "/big.bin", big);
    CHECK(host->Stat(bucket + "/big.bin", VFSFlags::F_ForceRefresh, {})->size == big.size());
    CHECK(ReadAll(host, bucket + "/big.bin") == big);

    REQUIRE(host->CreateDirectory(bucket + "/dir one", 0755, {}));
    WriteAll(host, bucket + "/dir one/ä+b (1).txt", "special");
    auto listing = host->FetchDirectoryListing(bucket + "/dir one", VFSFlags::F_ForceRefresh, {});
    REQUIRE(listing);
    CHECK(Names(**listing) == std::vector<std::string>{"..", "ä+b (1).txt"});

    REQUIRE(host->Rename(bucket + "/dir one", bucket + "/dir two", {}));
    CHECK(ReadAll(host, bucket + "/dir two/ä+b (1).txt") == "special");
    REQUIRE(host->Rename(bucket + "/small.txt", bucket + "/dir two/small.txt", {}));

    listing = host->FetchDirectoryListing(bucket, VFSFlags::F_ForceRefresh, {});
    REQUIRE(listing);
    CHECK(Names(**listing) == std::vector<std::string>{"..", "big.bin", "dir two/"});

    // cleanup
    REQUIRE(host->Unlink(bucket + "/dir two/ä+b (1).txt", {}));
    REQUIRE(host->Unlink(bucket + "/dir two/small.txt", {}));
    REQUIRE(host->RemoveDirectory(bucket + "/dir two", {}));
    REQUIRE(host->Unlink(bucket + "/big.bin", {}));
    REQUIRE(host->RemoveDirectory(bucket, {}));

    config.secret_key = "definitely-wrong";
    CHECK_THROWS_AS(std::make_shared<S3Host>(config), ErrorException);
}

} // namespace S3Tests
