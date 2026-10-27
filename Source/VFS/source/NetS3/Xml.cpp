// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Xml.h"
#include "Encoding.h"
#include <pugixml/pugixml.hpp>
#include <cstdlib>

namespace nc::vfs::s3 {

static bool Load(pugi::xml_document &_doc, std::string_view _xml)
{
    if( _xml.empty() )
        return false;
    const auto result = _doc.load_buffer(_xml.data(), _xml.size(), pugi::parse_default, pugi::encoding_utf8);
    return static_cast<bool>(result);
}

// S3 documents carry a default namespace, so elements are matched by their local name only.
static std::string_view LocalName(const char *_name) noexcept
{
    const std::string_view n(_name);
    const auto colon = n.find(':');
    return colon == std::string_view::npos ? n : n.substr(colon + 1);
}

static pugi::xml_node Child(const pugi::xml_node &_parent, std::string_view _name)
{
    for( pugi::xml_node c = _parent.first_child(); c; c = c.next_sibling() )
        if( c.type() == pugi::node_element && LocalName(c.name()) == _name )
            return c;
    return {};
}

static std::string Text(const pugi::xml_node &_parent, std::string_view _name)
{
    return Child(_parent, _name).text().as_string();
}

template <class F>
static void ForEachChild(const pugi::xml_node &_parent, std::string_view _name, F _f)
{
    for( pugi::xml_node c = _parent.first_child(); c; c = c.next_sibling() )
        if( c.type() == pugi::node_element && LocalName(c.name()) == _name )
            _f(c);
}

std::optional<std::vector<BucketEntry>> ParseListAllMyBuckets(std::string_view _xml)
{
    pugi::xml_document doc;
    if( !Load(doc, _xml) )
        return std::nullopt;
    const auto root = Child(doc, "ListAllMyBucketsResult");
    if( !root )
        return std::nullopt;

    std::vector<BucketEntry> buckets;
    ForEachChild(Child(root, "Buckets"), "Bucket", [&](const pugi::xml_node &_b) {
        BucketEntry e;
        e.name = Text(_b, "Name");
        e.creation_date = ParseIso8601(Text(_b, "CreationDate")).value_or(-1);
        if( !e.name.empty() )
            buckets.emplace_back(std::move(e));
    });
    return buckets;
}

std::optional<ListObjectsPage> ParseListObjectsV2(std::string_view _xml)
{
    pugi::xml_document doc;
    if( !Load(doc, _xml) )
        return std::nullopt;
    const auto root = Child(doc, "ListBucketResult");
    if( !root )
        return std::nullopt;

    const bool url_encoded = Text(root, "EncodingType") == "url";
    const auto decode = [url_encoded](std::string _s) { return url_encoded ? FormUrlDecode(_s) : _s; };

    ListObjectsPage page;
    page.is_truncated = Text(root, "IsTruncated") == "true";
    page.next_continuation_token = Text(root, "NextContinuationToken");

    ForEachChild(root, "Contents", [&](const pugi::xml_node &_c) {
        ObjectEntry e;
        e.key = decode(Text(_c, "Key"));
        e.size = std::strtoll(Text(_c, "Size").c_str(), nullptr, 10);
        e.last_modified = ParseIso8601(Text(_c, "LastModified")).value_or(-1);
        page.objects.emplace_back(std::move(e));
    });
    ForEachChild(root, "CommonPrefixes", [&](const pugi::xml_node &_p) {
        page.common_prefixes.emplace_back(decode(Text(_p, "Prefix")));
    });
    return page;
}

std::optional<ServiceError> ParseError(std::string_view _xml)
{
    pugi::xml_document doc;
    if( !Load(doc, _xml) )
        return std::nullopt;
    const auto root = Child(doc, "Error");
    if( !root )
        return std::nullopt;
    ServiceError e;
    e.code = Text(root, "Code");
    e.message = Text(root, "Message");
    e.region = Text(root, "Region");
    return e;
}

std::optional<std::string> ParseInitiateMultipartUpload(std::string_view _xml)
{
    pugi::xml_document doc;
    if( !Load(doc, _xml) )
        return std::nullopt;
    auto id = Text(Child(doc, "InitiateMultipartUploadResult"), "UploadId");
    if( id.empty() )
        return std::nullopt;
    return id;
}

std::optional<std::string> ParseCopyResultETag(std::string_view _xml)
{
    pugi::xml_document doc;
    if( !Load(doc, _xml) )
        return std::nullopt;
    for( const auto name : {"CopyObjectResult", "CopyPartResult"} )
        if( const auto root = Child(doc, name) ) {
            auto etag = Text(root, "ETag");
            if( !etag.empty() )
                return etag;
        }
    return std::nullopt;
}

static std::string XmlEscape(std::string_view _s)
{
    std::string out;
    out.reserve(_s.size());
    for( const char c : _s ) {
        switch( c ) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            default:
                out += c;
        }
    }
    return out;
}

std::string ComposeCompleteMultipartUpload(const std::vector<CompletedPart> &_parts)
{
    std::string xml = "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">";
    for( const auto &p : _parts ) {
        xml += "<Part><PartNumber>";
        xml += std::to_string(p.number);
        xml += "</PartNumber><ETag>";
        xml += XmlEscape(p.etag);
        xml += "</ETag></Part>";
    }
    xml += "</CompleteMultipartUpload>";
    return xml;
}

std::string ComposeCreateBucketConfiguration(std::string_view _region)
{
    return "<CreateBucketConfiguration xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\"><LocationConstraint>" +
           XmlEscape(_region) + "</LocationConstraint></CreateBucketConfiguration>";
}

} // namespace nc::vfs::s3
