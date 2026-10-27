// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Configuration.h"
#include "Addressing.h"
#include "S3Host.h"

namespace nc::vfs::s3 {

const char *HostConfiguration::Tag()
{
    return S3Host::UniqueTag;
}

const char *HostConfiguration::Junction() const
{
    return verbose.c_str();
}

const char *HostConfiguration::VerboseJunction() const
{
    return verbose.c_str();
}

bool HostConfiguration::operator==(const HostConfiguration &_rhs) const
{
    return endpoint == _rhs.endpoint && region == _rhs.region && bucket == _rhs.bucket &&
           access_key == _rhs.access_key && secret_key == _rhs.secret_key && session_token == _rhs.session_token &&
           path_style == _rhs.path_style;
}

void HostConfiguration::ComposeVerbose()
{
    std::string where;
    if( const auto ep = ParseEndpoint(endpoint) ) {
        where = ep->host;
        if( ep->port > 0 )
            where += ":" + std::to_string(ep->port);
    }
    else {
        where = "aws";
        if( !region.empty() )
            where += "-" + region;
    }
    verbose = "s3://" + where;
    if( !bucket.empty() )
        verbose += "/" + bucket;
}

} // namespace nc::vfs::s3
