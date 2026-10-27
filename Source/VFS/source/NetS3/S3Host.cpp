// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "S3Host.h"
#include "Cache.h"
#include "Client.h"
#include "CurlHttpClient.h"
#include "File.h"
#include "PathMapper.h"
#include "../ListingInput.h"
#include <VFS/VFSListing.h>
#include <sys/dirent.h>
#include <sys/stat.h>

namespace nc::vfs {

const char *S3Host::UniqueTag = "net_s3";

static constexpr uint16_t g_S3DirectoryMode = S_IRUSR | S_IWUSR | S_IXUSR | S_IFDIR;
static constexpr uint16_t g_S3RegularFileMode = S_IRUSR | S_IWUSR | S_IFREG;
static constexpr uint64_t g_S3SingleCopyLimit = 5ull * 1024 * 1024 * 1024; // CopyObject's hard limit
static constexpr uint64_t g_S3CopyPartSize = 512ull * 1024 * 1024;

struct S3Host::State {
    State(const s3::HostConfiguration &_config, std::shared_ptr<s3::HttpClientPool> _pool)
        : mapper(_config.bucket), client(MakeClientConfig(_config), std::move(_pool))
    {
    }

    static s3::ClientConfig MakeClientConfig(const s3::HostConfiguration &_config)
    {
        s3::ClientConfig c;
        c.credentials = s3::Credentials{_config.access_key, _config.secret_key, _config.session_token};
        c.region = _config.region.empty() ? "us-east-1" : _config.region;
        c.endpoint = s3::ParseEndpoint(_config.endpoint);
        c.path_style = _config.path_style;
        return c;
    }

    s3::PathMapper mapper;
    s3::Client client;
    s3::Cache cache;
};

static VFSConfiguration S3ComposeConfiguration(s3::HostConfiguration _config)
{
    _config.ComposeVerbose();
    return VFSConfiguration{std::move(_config)};
}

static VFSStat S3MakeStat(bool _is_directory, int64_t _size, std::time_t _mtime)
{
    VFSStat st;
    st.mode = _is_directory ? g_S3DirectoryMode : g_S3RegularFileMode;
    st.meaning.mode = true;
    if( !_is_directory && _size >= 0 ) {
        st.size = static_cast<uint64_t>(_size);
        st.meaning.size = true;
    }
    if( _mtime >= 0 ) {
        st.mtime.tv_sec = st.ctime.tv_sec = st.btime.tv_sec = _mtime;
        st.meaning.mtime = st.meaning.ctime = st.meaning.btime = true;
    }
    return st;
}

static Error S3PosixError(int _code)
{
    return Error{Error::POSIX, _code};
}

S3Host::S3Host(const s3::HostConfiguration &_config, std::shared_ptr<s3::HttpClientPool> _pool)
    : S3Host(S3ComposeConfiguration(_config), std::move(_pool))
{
}

S3Host::S3Host(const VFSConfiguration &_config, std::shared_ptr<s3::HttpClientPool> _pool)
    : Host(_config.Get<s3::HostConfiguration>().verbose, nullptr, UniqueTag), m_Configuration(_config)
{
    Init(std::move(_pool));
}

S3Host::~S3Host() = default;

void S3Host::Init(std::shared_ptr<s3::HttpClientPool> _pool)
{
    if( !_pool )
        _pool = std::make_shared<s3::HttpClientPool>([] { return std::make_unique<s3::CurlHttpClient>(); });
    I = std::make_unique<State>(Config(), std::move(_pool));

    // Fail early with a meaningful error if the credentials or the endpoint are wrong.
    std::expected<void, Error> probe;
    if( I->mapper.HasFixedBucket() ) {
        const auto page = I->client.ListObjects(Config().bucket, "", "/", "", 1);
        if( !page )
            probe = std::unexpected(page.error());
    }
    else {
        const auto buckets = I->client.ListBuckets();
        if( !buckets )
            probe = std::unexpected(buckets.error());
        else {
            std::vector<s3::Entry> entries;
            for( const auto &b : *buckets )
                entries.push_back(s3::Entry{b.name, true, -1, b.creation_date});
            I->cache.CommitListing("/", std::move(entries));
        }
    }
    if( !probe )
        throw ErrorException(probe.error());
}

VFSMeta S3Host::Meta()
{
    VFSMeta m;
    m.Tag = UniqueTag;
    m.SpawnWithConfig = []([[maybe_unused]] const VFSHostPtr &_parent,
                           const VFSConfiguration &_config,
                           [[maybe_unused]] VFSCancelChecker _cancel_checker) {
        return std::make_shared<S3Host>(_config);
    };
    return m;
}

VFSConfiguration S3Host::Configuration() const
{
    return m_Configuration;
}

const s3::HostConfiguration &S3Host::Config() const noexcept
{
    return m_Configuration.GetUnchecked<s3::HostConfiguration>();
}

s3::Client &S3Host::Client() noexcept
{
    return I->client;
}

s3::Cache &S3Host::Cache() noexcept
{
    return I->cache;
}

std::optional<s3::Location> S3Host::Map(std::string_view _path) const
{
    return I->mapper.Map(_path);
}

const std::string &S3Host::Endpoint() const noexcept
{
    return Config().endpoint;
}

const std::string &S3Host::Bucket() const noexcept
{
    return Config().bucket;
}

const std::string &S3Host::AccessKey() const noexcept
{
    return Config().access_key;
}

bool S3Host::IsWritable() const
{
    return true;
}

bool S3Host::IsCaseSensitiveAtPath([[maybe_unused]] std::string_view _dir) const
{
    return true;
}

std::expected<std::vector<s3::Entry>, Error>
S3Host::ListDirectory(const std::string &_dir_path, bool _force_refresh, const VFSCancelChecker &_cancel_checker)
{
    if( _force_refresh )
        I->cache.Discard(_dir_path);
    else if( auto cached = I->cache.Listing(_dir_path) )
        return std::move(*cached);

    const auto location = Map(_dir_path);
    if( !location )
        return std::unexpected(S3PosixError(EINVAL));

    std::vector<s3::Entry> entries;
    if( location->IsServiceRoot() ) {
        const auto buckets = I->client.ListBuckets(_cancel_checker);
        if( !buckets )
            return std::unexpected(buckets.error());
        for( const auto &b : *buckets )
            entries.push_back(s3::Entry{b.name, true, -1, b.creation_date});
    }
    else {
        const std::string prefix = location->DirectoryPrefix();
        const auto rc = I->client.ListAllObjects(
            location->bucket,
            prefix,
            "/",
            [&](const s3::Client::Page &_page) {
                for( const auto &object : _page.objects ) {
                    if( object.key.size() <= prefix.size() )
                        continue; // the "directory marker" object itself
                    std::string name = object.key.substr(prefix.size());
                    if( name.find('/') != std::string::npos )
                        continue;
                    entries.push_back(s3::Entry{std::move(name), false, object.size, object.last_modified});
                }
                for( const auto &common : _page.common_prefixes ) {
                    if( common.size() <= prefix.size() + 1 )
                        continue; // an empty path component, e.g. "a//b"
                    std::string name = common.substr(prefix.size(), common.size() - prefix.size() - 1);
                    if( name.find('/') != std::string::npos )
                        continue;
                    entries.push_back(s3::Entry{std::move(name), true, -1, -1});
                }
                return true;
            },
            _cancel_checker);
        if( !rc )
            return std::unexpected(rc.error());
    }

    I->cache.CommitListing(_dir_path, entries);
    if( auto cached = I->cache.Listing(_dir_path) ) // the cache sorts and de-duplicates the entries
        return std::move(*cached);
    return entries;
}

std::expected<VFSListingPtr, Error>
S3Host::FetchDirectoryListing(std::string_view _path, unsigned long _flags, const VFSCancelChecker &_cancel_checker)
{
    const std::string path = s3::EnsureTrailingSlash(_path);
    auto entries = ListDirectory(path, (_flags & VFSFlags::F_ForceRefresh) != 0, _cancel_checker);
    if( !entries )
        return std::unexpected(entries.error());

    using nc::base::variable_container;
    ListingInput listing;
    listing.hosts[0] = shared_from_this();
    listing.directories[0] = path;
    listing.sizes.reset(variable_container<>::type::dense);
    listing.mtimes.reset(variable_container<>::type::sparse);
    listing.ctimes.reset(variable_container<>::type::sparse);
    listing.btimes.reset(variable_container<>::type::sparse);

    int index = 0;
    const auto add = [&](const std::string &_name, bool _is_dir, int64_t _size, std::time_t _mtime) {
        listing.filenames.emplace_back(_name);
        listing.unix_modes.emplace_back(_is_dir ? g_S3DirectoryMode : g_S3RegularFileMode);
        listing.unix_types.emplace_back(_is_dir ? DT_DIR : DT_REG);
        listing.sizes.insert(index, (_is_dir || _size < 0) ? ListingInput::unknown_size : static_cast<uint64_t>(_size));
        if( _mtime >= 0 ) {
            listing.mtimes.insert(index, _mtime);
            listing.ctimes.insert(index, _mtime);
            listing.btimes.insert(index, _mtime);
        }
        ++index;
    };

    if( !(_flags & VFSFlags::F_NoDotDot) && path != "/" )
        add("..", true, -1, -1);
    for( const auto &e : *entries )
        add(e.name, e.is_directory, e.size, e.mtime);

    return VFSListing::Build(std::move(listing));
}

std::expected<void, Error>
S3Host::IterateDirectoryListing(std::string_view _path, const std::function<bool(const VFSDirEnt &_dirent)> &_handler)
{
    const auto entries = ListDirectory(s3::EnsureTrailingSlash(_path), false, nullptr);
    if( !entries )
        return std::unexpected(entries.error());
    for( const auto &e : *entries ) {
        VFSDirEnt dirent;
        dirent.name = e.name;
        dirent.type = e.is_directory ? VFSDirEnt::Dir : VFSDirEnt::Reg;
        if( !_handler(dirent) )
            return std::unexpected(S3PosixError(ECANCELED));
    }
    return {};
}

std::expected<VFSStat, Error>
S3Host::Stat(std::string_view _path, [[maybe_unused]] unsigned long _flags, const VFSCancelChecker &_cancel_checker)
{
    const auto location = Map(_path);
    if( !location )
        return std::unexpected(S3PosixError(EINVAL));
    if( location->IsServiceRoot() || location->IsBucketRoot() )
        return S3MakeStat(true, -1, -1);

    const std::string path(_path);
    if( const auto lookup = I->cache.Item(path); lookup.parent_cached ) {
        if( !lookup.entry )
            return std::unexpected(S3PosixError(ENOENT));
        return S3MakeStat(lookup.entry->is_directory, lookup.entry->size, lookup.entry->mtime);
    }

    // Not cached - ask the server: an object first, then a "directory" (any key under the prefix).
    if( !path.ends_with('/') ) {
        const auto info = I->client.HeadObject(location->bucket, location->key, _cancel_checker);
        if( info )
            return S3MakeStat(false, info->size, info->last_modified);
        if( info.error() != S3PosixError(ENOENT) && info.error() != S3PosixError(EACCES) )
            return std::unexpected(info.error());
    }

    const auto page =
        I->client.ListObjects(location->bucket, location->DirectoryPrefix(), "/", "", 1, _cancel_checker);
    if( !page )
        return std::unexpected(page.error());
    if( !page->objects.empty() || !page->common_prefixes.empty() )
        return S3MakeStat(true, -1, -1);
    return std::unexpected(S3PosixError(ENOENT));
}

std::expected<VFSStatFS, Error> S3Host::StatFS([[maybe_unused]] std::string_view _path,
                                               [[maybe_unused]] const VFSCancelChecker &_cancel_checker)
{
    VFSStatFS stat;
    stat.volume_name = Config().verbose;
    return stat;
}

std::expected<void, Error>
S3Host::CreateDirectory(std::string_view _path, [[maybe_unused]] int _mode, const VFSCancelChecker &_cancel_checker)
{
    const auto location = Map(_path);
    if( !location )
        return std::unexpected(S3PosixError(EINVAL));
    if( location->IsServiceRoot() || (location->IsBucketRoot() && I->mapper.HasFixedBucket()) )
        return std::unexpected(S3PosixError(EEXIST));

    if( location->IsBucketRoot() ) {
        if( const auto rc = I->client.CreateBucket(location->bucket, _cancel_checker); !rc )
            return rc;
    }
    else {
        const std::string marker = location->DirectoryPrefix();
        if( const auto rc = I->client.PutObject(location->bucket, marker, {}, _cancel_checker); !rc )
            return rc;
    }

    const auto split = s3::SplitParent(_path);
    if( split ) {
        I->cache.CommitUpsert(std::string(_path), s3::Entry{split->second, true, -1, std::time(nullptr)});
        I->cache.CommitListing(s3::EnsureTrailingSlash(_path), {});
    }
    return {};
}

std::expected<void, Error> S3Host::RemoveDirectory(std::string_view _path, const VFSCancelChecker &_cancel_checker)
{
    const auto location = Map(_path);
    if( !location )
        return std::unexpected(S3PosixError(EINVAL));
    if( location->IsServiceRoot() || (location->IsBucketRoot() && I->mapper.HasFixedBucket()) )
        return std::unexpected(S3PosixError(EPERM));

    if( location->IsBucketRoot() ) {
        if( const auto rc = I->client.DeleteBucket(location->bucket, _cancel_checker); !rc )
            return rc;
    }
    else {
        // POSIX semantics: refuse to remove a directory that still has contents.
        const auto page =
            I->client.ListObjects(location->bucket, location->DirectoryPrefix(), "/", "", 2, _cancel_checker);
        if( !page )
            return std::unexpected(page.error());
        for( const auto &object : page->objects )
            if( object.key != location->DirectoryPrefix() )
                return std::unexpected(S3PosixError(ENOTEMPTY));
        if( !page->common_prefixes.empty() )
            return std::unexpected(S3PosixError(ENOTEMPTY));

        if( const auto rc = I->client.DeleteObject(location->bucket, location->DirectoryPrefix(), _cancel_checker);
            !rc )
            return rc;
    }

    I->cache.DiscardSubtree(s3::EnsureTrailingSlash(_path));
    I->cache.CommitRemove(std::string(_path));
    return {};
}

std::expected<void, Error> S3Host::Unlink(std::string_view _path, const VFSCancelChecker &_cancel_checker)
{
    const auto location = Map(_path);
    if( !location || location->key.empty() )
        return std::unexpected(S3PosixError(EINVAL));
    if( const auto rc = I->client.DeleteObject(location->bucket, location->key, _cancel_checker); !rc )
        return rc;
    I->cache.CommitRemove(std::string(_path));
    return {};
}

std::expected<std::shared_ptr<VFSFile>, Error> S3Host::CreateFile(std::string_view _path,
                                                                  [[maybe_unused]] const VFSCancelChecker &_cancel)
{
    const auto location = Map(_path);
    if( !location || location->key.empty() )
        return std::unexpected(S3PosixError(EINVAL));
    return std::make_shared<s3::File>(_path, std::static_pointer_cast<S3Host>(shared_from_this()));
}

std::expected<void, Error> S3Host::CopyObject(const s3::Location &_from,
                                              const s3::Location &_to,
                                              uint64_t _size,
                                              const VFSCancelChecker &_cancel_checker)
{
    if( _size <= g_S3SingleCopyLimit )
        return I->client.CopyObject(_from.bucket, _from.key, _to.bucket, _to.key, _cancel_checker);

    const auto upload_id = I->client.CreateMultipartUpload(_to.bucket, _to.key, _cancel_checker);
    if( !upload_id )
        return std::unexpected(upload_id.error());

    std::vector<s3::CompletedPart> parts;
    int number = 1;
    for( uint64_t offset = 0; offset < _size; offset += g_S3CopyPartSize, ++number ) {
        const uint64_t last = std::min(offset + g_S3CopyPartSize, _size) - 1;
        auto etag = I->client.UploadPartCopy(
            _from.bucket, _from.key, offset, last, _to.bucket, _to.key, *upload_id, number, _cancel_checker);
        if( !etag ) {
            std::ignore = I->client.AbortMultipartUpload(_to.bucket, _to.key, *upload_id);
            return std::unexpected(etag.error());
        }
        parts.push_back(s3::CompletedPart{number, std::move(*etag)});
    }
    const auto rc = I->client.CompleteMultipartUpload(_to.bucket, _to.key, *upload_id, parts, _cancel_checker);
    if( !rc )
        std::ignore = I->client.AbortMultipartUpload(_to.bucket, _to.key, *upload_id);
    return rc;
}

std::expected<void, Error>
S3Host::RenameDirectory(const s3::Location &_from, const s3::Location &_to, const VFSCancelChecker &_cancel_checker)
{
    const std::string from_prefix = _from.DirectoryPrefix();
    const std::string to_prefix = _to.DirectoryPrefix();
    if( to_prefix.starts_with(from_prefix) && _from.bucket == _to.bucket )
        return std::unexpected(S3PosixError(EINVAL)); // moving a directory into itself

    // Collect first, then mutate, so that the listing isn't affected by the copies being made.
    std::vector<s3::ObjectEntry> objects;
    const auto listed = I->client.ListAllObjects(
        _from.bucket,
        from_prefix,
        "",
        [&](const s3::Client::Page &_page) {
            objects.insert(objects.end(), _page.objects.begin(), _page.objects.end());
            return true;
        },
        _cancel_checker);
    if( !listed )
        return listed;

    for( const auto &object : objects ) {
        if( _cancel_checker && _cancel_checker() )
            return std::unexpected(S3PosixError(ECANCELED));
        const s3::Location src{_from.bucket, object.key};
        const s3::Location dst{_to.bucket, to_prefix + object.key.substr(from_prefix.size())};
        if( const auto rc = CopyObject(src, dst, static_cast<uint64_t>(object.size), _cancel_checker); !rc )
            return rc;
        if( const auto rc = I->client.DeleteObject(src.bucket, src.key, _cancel_checker); !rc )
            return rc;
    }
    if( objects.empty() ) // an implicit empty directory - materialize it at the destination
        return I->client.PutObject(_to.bucket, to_prefix, {}, _cancel_checker);
    return {};
}

std::expected<void, Error>
S3Host::Rename(std::string_view _old_path, std::string_view _new_path, const VFSCancelChecker &_cancel_checker)
{
    const auto from = Map(_old_path);
    const auto to = Map(_new_path);
    if( !from || !to || from->key.empty() || to->key.empty() )
        return std::unexpected(S3PosixError(from && from->IsBucketRoot() ? ENOTSUP : EINVAL));

    const auto st = Stat(_old_path, 0, _cancel_checker);
    if( !st )
        return std::unexpected(st.error());

    std::expected<void, Error> rc;
    if( st->mode_bits.dir ) {
        rc = RenameDirectory(*from, *to, _cancel_checker);
    }
    else {
        rc = CopyObject(*from, *to, st->size, _cancel_checker);
        if( rc )
            rc = I->client.DeleteObject(from->bucket, from->key, _cancel_checker);
    }

    // Renames can touch many keys; simply invalidate everything around both locations.
    I->cache.DiscardSubtree(s3::EnsureTrailingSlash(_old_path));
    I->cache.DiscardSubtree(s3::EnsureTrailingSlash(_new_path));
    for( const auto path : {_old_path, _new_path} )
        if( const auto split = s3::SplitParent(path) ) {
            I->cache.Discard(split->first);
            I->cache.Notify(split->first);
        }
    return rc;
}

bool S3Host::IsDirectoryChangeObservationAvailable([[maybe_unused]] std::string_view _path)
{
    return true;
}

HostDirObservationTicket S3Host::ObserveDirectoryChanges(std::string_view _path, std::function<void()> _handler)
{
    if( !Map(_path) )
        return {};
    const auto ticket = I->cache.Observe(s3::EnsureTrailingSlash(_path), std::move(_handler));
    return HostDirObservationTicket{static_cast<unsigned long>(ticket), shared_from_this()};
}

void S3Host::StopDirChangeObserving(unsigned long _ticket)
{
    I->cache.StopObserving(_ticket);
}

} // namespace nc::vfs
