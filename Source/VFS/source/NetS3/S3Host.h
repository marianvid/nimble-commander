// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include "../../include/VFS/Host.h"
#include "Configuration.h"

namespace nc::vfs {

namespace s3 {
class Client;
class Cache;
class HttpClientPool;
class PathMapper;
struct Location;
struct Entry;
} // namespace s3

// Virtual file system over Amazon S3 and S3-compatible object storages (MinIO, Cloudflare R2, Backblaze B2, ...).
// Keys are presented as a directory tree using "/" as the delimiter.
class S3Host final : public Host
{
public:
    static const char *UniqueTag;

    // Verifies the credentials by issuing a lightweight request; throws ErrorException on failure.
    // _pool allows injecting a custom transport (used by tests); by default libcurl is used.
    explicit S3Host(const s3::HostConfiguration &_config, std::shared_ptr<s3::HttpClientPool> _pool = nullptr);
    explicit S3Host(const VFSConfiguration &_config, std::shared_ptr<s3::HttpClientPool> _pool = nullptr);
    ~S3Host() override;

    static VFSMeta Meta();

    VFSConfiguration Configuration() const override;

    bool IsWritable() const override;
    bool IsCaseSensitiveAtPath(std::string_view _dir = "/") const override;

    std::expected<VFSListingPtr, Error> FetchDirectoryListing(std::string_view _path,
                                                              unsigned long _flags,
                                                              const VFSCancelChecker &_cancel_checker) override;

    std::expected<void, Error>
    IterateDirectoryListing(std::string_view _path,
                            const std::function<bool(const VFSDirEnt &_dirent)> &_handler) override;

    std::expected<VFSStat, Error>
    Stat(std::string_view _path, unsigned long _flags, const VFSCancelChecker &_cancel_checker) override;

    std::expected<VFSStatFS, Error> StatFS(std::string_view _path, const VFSCancelChecker &_cancel_checker) override;

    std::expected<void, Error>
    CreateDirectory(std::string_view _path, int _mode, const VFSCancelChecker &_cancel_checker) override;

    std::expected<void, Error> RemoveDirectory(std::string_view _path,
                                               const VFSCancelChecker &_cancel_checker) override;

    std::expected<void, Error> Unlink(std::string_view _path, const VFSCancelChecker &_cancel_checker) override;

    std::expected<std::shared_ptr<VFSFile>, Error> CreateFile(std::string_view _path,
                                                              const VFSCancelChecker &_cancel_checker) override;

    std::expected<void, Error>
    Rename(std::string_view _old_path, std::string_view _new_path, const VFSCancelChecker &_cancel_checker) override;

    bool IsDirectoryChangeObservationAvailable(std::string_view _path) override;
    HostDirObservationTicket ObserveDirectoryChanges(std::string_view _path, std::function<void()> _handler) override;

    // Accessors used by the file implementation and by the UI layer.
    [[nodiscard]] const s3::HostConfiguration &Config() const noexcept;
    [[nodiscard]] s3::Client &Client() noexcept;
    [[nodiscard]] s3::Cache &Cache() noexcept;
    [[nodiscard]] std::optional<s3::Location> Map(std::string_view _path) const;

    [[nodiscard]] const std::string &Endpoint() const noexcept;
    [[nodiscard]] const std::string &Bucket() const noexcept;
    [[nodiscard]] const std::string &AccessKey() const noexcept;

    // Server-side copy of an object of a known size, choosing between a single request and a multipart copy.
    std::expected<void, Error> CopyObject(const s3::Location &_from,
                                          const s3::Location &_to,
                                          uint64_t _size,
                                          const VFSCancelChecker &_cancel_checker);

private:
    void Init(std::shared_ptr<s3::HttpClientPool> _pool);
    void StopDirChangeObserving(unsigned long _ticket) override;

    std::expected<std::vector<s3::Entry>, Error> ListDirectory(const std::string &_dir_path,
                                                               bool _force_refresh,
                                                               const VFSCancelChecker &_cancel_checker);
    std::expected<void, Error> RenameDirectory(const s3::Location &_from,
                                               const s3::Location &_to,
                                               const VFSCancelChecker &_cancel_checker);

    struct State;
    std::unique_ptr<State> I;
    VFSConfiguration m_Configuration;
};

} // namespace nc::vfs
