// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "PathMapper.h"
#include <vector>

namespace nc::vfs::s3 {

std::string Location::DirectoryPrefix() const
{
    if( key.empty() || key.back() == '/' )
        return key;
    return key + "/";
}

PathMapper::PathMapper(std::string _fixed_bucket) : m_FixedBucket(std::move(_fixed_bucket))
{
}

std::optional<Location> PathMapper::Map(std::string_view _vfs_path) const
{
    if( _vfs_path.empty() || _vfs_path.front() != '/' )
        return std::nullopt;

    std::vector<std::string_view> parts;
    std::string_view rest = _vfs_path.substr(1);
    while( !rest.empty() ) {
        const auto slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if( part.empty() ) {
            if( slash == std::string_view::npos || slash + 1 == rest.size() )
                break; // a trailing slash is fine
            return std::nullopt; // "//" inside the path
        }
        if( part == "." || part == ".." )
            return std::nullopt;
        parts.push_back(part);
        if( slash == std::string_view::npos )
            break;
        rest.remove_prefix(slash + 1);
    }

    Location loc;
    size_t first_key_part = 0;
    if( HasFixedBucket() ) {
        loc.bucket = m_FixedBucket;
    }
    else {
        if( parts.empty() )
            return loc; // the service root
        loc.bucket = std::string(parts.front());
        first_key_part = 1;
    }

    for( size_t i = first_key_part; i < parts.size(); ++i ) {
        if( !loc.key.empty() )
            loc.key += '/';
        loc.key += parts[i];
    }
    return loc;
}

std::string PathMapper::ToVFSDirectory(const Location &_location) const
{
    std::string path = "/";
    if( !HasFixedBucket() && !_location.bucket.empty() )
        path += _location.bucket + "/";
    path += _location.DirectoryPrefix();
    return path;
}

std::string EnsureTrailingSlash(std::string_view _path)
{
    std::string p(_path);
    if( p.empty() || p.back() != '/' )
        p += '/';
    return p;
}

std::optional<std::pair<std::string, std::string>> SplitParent(std::string_view _path)
{
    if( _path.empty() || _path.front() != '/' )
        return std::nullopt;
    if( _path.size() > 1 && _path.back() == '/' )
        _path.remove_suffix(1);
    if( _path == "/" )
        return std::nullopt;
    const auto slash = _path.rfind('/');
    return std::pair<std::string, std::string>{std::string(_path.substr(0, slash + 1)),
                                               std::string(_path.substr(slash + 1))};
}

} // namespace nc::vfs::s3
