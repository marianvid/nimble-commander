// Copyright (C) 2026 Marian Vid. Subject to GNU General Public License version 3.
#include "Cache.h"
#include "PathMapper.h"
#include <algorithm>

namespace nc::vfs::s3 {

static bool EntryNameLess(const Entry &_lhs, const Entry &_rhs) noexcept
{
    return _lhs.name < _rhs.name;
}

Cache::Cache(std::chrono::seconds _ttl) : m_TTL(_ttl)
{
}

bool Cache::IsFresh(const Directory &_dir) const noexcept
{
    return Clock::now() - _dir.fetched < m_TTL;
}

std::optional<std::vector<Entry>> Cache::Listing(const std::string &_dir) const
{
    const std::lock_guard lock{m_Lock};
    const auto it = m_Dirs.find(_dir);
    if( it == m_Dirs.end() || !IsFresh(it->second) )
        return std::nullopt;
    return it->second.entries;
}

Cache::Lookup Cache::Item(const std::string &_path) const
{
    const auto split = SplitParent(_path);
    if( !split )
        return {};
    const std::lock_guard lock{m_Lock};
    const auto it = m_Dirs.find(split->first);
    if( it == m_Dirs.end() || !IsFresh(it->second) )
        return {};

    Lookup result;
    result.parent_cached = true;
    const auto &entries = it->second.entries;
    const Entry probe{split->second, false, -1, -1};
    const auto pos = std::ranges::lower_bound(entries, probe, EntryNameLess);
    if( pos != entries.end() && pos->name == split->second )
        result.entry = *pos;
    return result;
}

void Cache::CommitListing(const std::string &_dir, std::vector<Entry> _entries)
{
    // Both an object "x" and a prefix "x/" may exist; keep the directory, it's the more useful view.
    std::ranges::sort(_entries, [](const Entry &a, const Entry &b) {
        return a.name != b.name ? a.name < b.name : (a.is_directory && !b.is_directory);
    });
    const auto dup = std::ranges::unique(_entries, [](const Entry &a, const Entry &b) { return a.name == b.name; });
    _entries.erase(dup.begin(), dup.end());

    const std::lock_guard lock{m_Lock};
    auto &dir = m_Dirs[_dir];
    dir.fetched = Clock::now();
    dir.entries = std::move(_entries);
}

void Cache::Discard(const std::string &_dir)
{
    const std::lock_guard lock{m_Lock};
    m_Dirs.erase(_dir);
}

void Cache::DiscardSubtree(const std::string &_dir)
{
    const std::lock_guard lock{m_Lock};
    std::erase_if(m_Dirs, [&](const auto &_item) { return _item.first.starts_with(_dir); });
}

void Cache::CommitUpsert(const std::string &_path, const Entry &_entry)
{
    const auto split = SplitParent(_path);
    if( !split )
        return;
    {
        const std::lock_guard lock{m_Lock};
        if( const auto it = m_Dirs.find(split->first); it != m_Dirs.end() ) {
            auto &entries = it->second.entries;
            const auto pos = std::ranges::lower_bound(entries, _entry, EntryNameLess);
            if( pos != entries.end() && pos->name == _entry.name ) {
                // Never downgrade a directory into a file - both may legitimately coexist in S3.
                if( !pos->is_directory || _entry.is_directory )
                    *pos = _entry;
            }
            else {
                entries.insert(pos, _entry);
            }
        }
    }
    Notify(split->first);
}

void Cache::CommitRemove(const std::string &_path)
{
    const auto split = SplitParent(_path);
    if( !split )
        return;
    {
        const std::lock_guard lock{m_Lock};
        if( const auto it = m_Dirs.find(split->first); it != m_Dirs.end() ) {
            auto &entries = it->second.entries;
            const Entry probe{split->second, false, -1, -1};
            const auto pos = std::ranges::lower_bound(entries, probe, EntryNameLess);
            if( pos != entries.end() && pos->name == split->second )
                entries.erase(pos);
        }
    }
    Notify(split->first);
}

void Cache::Notify(const std::string &_dir)
{
    std::vector<std::function<void()>> callbacks;
    {
        const std::lock_guard lock{m_ObserversLock};
        const auto [first, last] = m_Observers.equal_range(_dir);
        for( auto it = first; it != last; ++it )
            callbacks.emplace_back(it->second.callback);
    }
    for( const auto &cb : callbacks )
        cb();
}

uint64_t Cache::Observe(const std::string &_dir, std::function<void()> _handler)
{
    const uint64_t ticket = ++m_LastTicket;
    const std::lock_guard lock{m_ObserversLock};
    m_Observers.emplace(_dir, Observer{ticket, std::move(_handler)});
    return ticket;
}

void Cache::StopObserving(uint64_t _ticket)
{
    const std::lock_guard lock{m_ObserversLock};
    std::erase_if(m_Observers, [_ticket](const auto &_item) { return _item.second.ticket == _ticket; });
}

} // namespace nc::vfs::s3
