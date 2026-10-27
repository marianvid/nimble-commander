// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace nc::vfs::s3 {

// One entry of a directory-like view over S3: either an object or a common prefix ("directory").
struct Entry {
    std::string name; // single path component, never contains '/'
    bool is_directory = false;
    int64_t size = -1;
    std::time_t mtime = -1;
};

// Caches directory listings keyed by VFS directory paths (always with a trailing slash) and
// notifies observers when a directory's contents are known to have changed.
// Every listing is kept sorted by name. Thread-safe.
class Cache
{
public:
    using Clock = std::chrono::steady_clock;

    explicit Cache(std::chrono::seconds _ttl = std::chrono::seconds{30});

    std::optional<std::vector<Entry>> Listing(const std::string &_dir) const;

    // Looks up a single item via its parent's cached listing.
    // Returns nullopt if the parent isn't cached; {nullopt-entry, true} means "known not to exist".
    struct Lookup {
        bool parent_cached = false;
        std::optional<Entry> entry;
    };
    Lookup Item(const std::string &_path) const;

    void CommitListing(const std::string &_dir, std::vector<Entry> _entries);
    void Discard(const std::string &_dir);
    void DiscardSubtree(const std::string &_dir);

    // Mutations of a cached parent listing (no-ops if the parent isn't cached). They also notify observers.
    void CommitUpsert(const std::string &_path, const Entry &_entry);
    void CommitRemove(const std::string &_path);

    // Notifies observers of _dir without touching the cache.
    void Notify(const std::string &_dir);

    uint64_t Observe(const std::string &_dir, std::function<void()> _handler);
    void StopObserving(uint64_t _ticket);

private:
    struct Directory {
        Clock::time_point fetched;
        std::vector<Entry> entries;
    };
    struct Observer {
        uint64_t ticket;
        std::function<void()> callback;
    };

    [[nodiscard]] bool IsFresh(const Directory &_dir) const noexcept;

    std::chrono::seconds m_TTL;
    std::unordered_map<std::string, Directory> m_Dirs;
    mutable std::mutex m_Lock;

    std::atomic<uint64_t> m_LastTicket{0};
    std::unordered_multimap<std::string, Observer> m_Observers;
    mutable std::mutex m_ObserversLock;
};

} // namespace nc::vfs::s3
