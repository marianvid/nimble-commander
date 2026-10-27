// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "AwsProfiles.h"
#include "Encoding.h"
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <map>
#include <pwd.h>
#include <sstream>
#include <unistd.h>

namespace nc::vfs::s3 {

using AwsIniSections = std::map<std::string, std::map<std::string, std::string>>;

static AwsIniSections ParseAwsIni(std::string_view _ini, bool _config_style)
{
    AwsIniSections sections;
    std::string current;
    bool in_profile = false;
    size_t pos = 0;
    while( pos <= _ini.size() ) {
        auto eol = _ini.find('\n', pos);
        if( eol == std::string_view::npos )
            eol = _ini.size();
        const std::string_view line = TrimWhitespace(_ini.substr(pos, eol - pos));
        pos = eol + 1;

        if( line.empty() || line.front() == '#' || line.front() == ';' )
            continue;
        if( line.front() == '[' && line.back() == ']' ) {
            std::string_view name = TrimWhitespace(line.substr(1, line.size() - 2));
            in_profile = true;
            if( _config_style ) {
                if( name.starts_with("profile ") )
                    name = TrimWhitespace(name.substr(8));
                else if( name != "default" )
                    in_profile = false; // e.g. [sso-session x] or [services x]
            }
            current = std::string(name);
            continue;
        }
        if( !in_profile || current.empty() )
            continue;
        const auto eq = line.find('=');
        if( eq == std::string_view::npos )
            continue;
        const std::string key(TrimWhitespace(line.substr(0, eq)));
        const std::string value(TrimWhitespace(line.substr(eq + 1)));
        sections[current][key] = value;
    }
    return sections;
}

std::vector<AwsProfile> ParseAwsProfiles(std::string_view _credentials_ini, std::string_view _config_ini)
{
    const AwsIniSections credentials = ParseAwsIni(_credentials_ini, false);
    const AwsIniSections config = ParseAwsIni(_config_ini, true);

    std::map<std::string, AwsProfile> merged;
    const auto apply = [&](const AwsIniSections &_sections) {
        for( const auto &[name, values] : _sections ) {
            auto &p = merged[name];
            p.name = name;
            const auto get = [&](const char *_key, std::string &_out) {
                if( const auto it = values.find(_key); it != values.end() && !it->second.empty() )
                    _out = it->second;
            };
            get("aws_access_key_id", p.access_key);
            get("aws_secret_access_key", p.secret_key);
            get("aws_session_token", p.session_token);
            get("region", p.region);
            get("endpoint_url", p.endpoint);
        }
    };
    apply(config);
    apply(credentials); // the credentials file has precedence, as in the AWS CLI

    std::vector<AwsProfile> profiles;
    for( auto &[name, p] : merged )
        profiles.emplace_back(std::move(p));
    std::ranges::stable_partition(profiles, [](const AwsProfile &_p) { return _p.name == "default"; });
    return profiles;
}

static std::string AwsHomeDirectory()
{
    if( const char *home = std::getenv("HOME"); home != nullptr && *home != 0 )
        return home;
    if( const passwd *pw = getpwuid(getuid()); pw != nullptr && pw->pw_dir != nullptr )
        return pw->pw_dir;
    return {};
}

static std::string AwsReadTextFile(const std::string &_path)
{
    std::ifstream in(_path, std::ios::binary);
    if( !in )
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string AwsFileFromEnvOr(const char *_env, const std::string &_fallback)
{
    if( const char *v = std::getenv(_env); v != nullptr && *v != 0 )
        return v;
    return _fallback;
}

std::vector<AwsProfile> LoadAwsProfiles()
{
    const std::string home = AwsHomeDirectory();
    const std::string credentials =
        AwsReadTextFile(AwsFileFromEnvOr("AWS_SHARED_CREDENTIALS_FILE", home + "/.aws/credentials"));
    const std::string config = AwsReadTextFile(AwsFileFromEnvOr("AWS_CONFIG_FILE", home + "/.aws/config"));
    return ParseAwsProfiles(credentials, config);
}

std::optional<AwsProfile> LoadAwsProfile(std::string_view _name)
{
    for( auto &p : LoadAwsProfiles() )
        if( p.name == _name )
            return std::move(p);
    return std::nullopt;
}

} // namespace nc::vfs::s3
