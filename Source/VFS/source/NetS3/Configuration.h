// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <string>

namespace nc::vfs::s3 {

// Everything needed to spawn an S3 host. Stored inside VFSConfiguration.
class HostConfiguration
{
public:
    std::string endpoint; // empty means AWS; otherwise "https://host[:port]" of an S3-compatible service
    std::string region;   // e.g. "eu-central-1"; empty is treated as "us-east-1"
    std::string bucket;   // optional; when set, the host is rooted at this bucket
    std::string access_key;
    std::string secret_key;
    std::string session_token; // optional, for temporary credentials
    bool path_style = false;

    std::string verbose; // cached human-readable address, e.g. "s3://minio.lan:9000/photos"

    static const char *Tag();
    [[nodiscard]] const char *Junction() const;
    [[nodiscard]] const char *VerboseJunction() const;
    bool operator==(const HostConfiguration &_rhs) const;

    // Fills the "verbose" field from the other ones.
    void ComposeVerbose();
};

} // namespace nc::vfs::s3
