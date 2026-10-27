// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nc::vfs::s3 {

// A named profile from the AWS CLI shared configuration files.
struct AwsProfile {
    std::string name;
    std::string access_key;
    std::string secret_key;
    std::string session_token;
    std::string region;
    std::string endpoint; // "endpoint_url", supported by recent AWS CLI versions for S3-compatible services
    bool operator==(const AwsProfile &) const noexcept = default;
};

// Merges profiles from the contents of "~/.aws/credentials" and "~/.aws/config".
// The credentials file uses "[name]" sections, the config file uses "[profile name]" and "[default]".
// Profiles are returned sorted by name, "default" first.
std::vector<AwsProfile> ParseAwsProfiles(std::string_view _credentials_ini, std::string_view _config_ini);

// Reads the shared files of the current user, honoring AWS_SHARED_CREDENTIALS_FILE and AWS_CONFIG_FILE.
std::vector<AwsProfile> LoadAwsProfiles();

std::optional<AwsProfile> LoadAwsProfile(std::string_view _name);

} // namespace nc::vfs::s3
