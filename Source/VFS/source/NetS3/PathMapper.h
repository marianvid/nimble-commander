// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace nc::vfs::s3 {

// A position inside the S3 namespace.
struct Location {
    std::string bucket; // empty only for the service root (the list of buckets)
    std::string key;    // object key or key prefix without a leading slash; empty for a bucket root

    [[nodiscard]] bool IsServiceRoot() const noexcept { return bucket.empty(); }
    [[nodiscard]] bool IsBucketRoot() const noexcept { return !bucket.empty() && key.empty(); }

    // The key prefix that addresses the contents of this location as a directory: "a/b/" or "" for a bucket root.
    [[nodiscard]] std::string DirectoryPrefix() const;

    bool operator==(const Location &) const noexcept = default;
};

// Translates VFS paths into S3 locations.
// Without a fixed bucket: "/" lists buckets, "/bucket/a/b" is key "a/b" in "bucket".
// With a fixed bucket:    "/" is the bucket root, "/a/b" is key "a/b".
class PathMapper
{
public:
    explicit PathMapper(std::string _fixed_bucket);

    // Returns nullopt for paths that are not absolute or contain empty / dot components.
    [[nodiscard]] std::optional<Location> Map(std::string_view _vfs_path) const;

    [[nodiscard]] bool HasFixedBucket() const noexcept { return !m_FixedBucket.empty(); }

    // Builds a VFS directory path (with a trailing slash) for a location.
    [[nodiscard]] std::string ToVFSDirectory(const Location &_location) const;

private:
    std::string m_FixedBucket;
};

// Ensures a single trailing slash: "/a/b" -> "/a/b/", "/" -> "/".
std::string EnsureTrailingSlash(std::string_view _path);

// Splits "/a/b/c" or "/a/b/c/" into {"/a/b/", "c"}. Returns nullopt for "/" or relative paths.
std::optional<std::pair<std::string, std::string>> SplitParent(std::string_view _path);

} // namespace nc::vfs::s3
