/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Readers' answers that are the same for everyone, kept in memory until what they show changes: the
 * published posts' lists and pages, the comments of a post or the guestbook as readers
 * see them (their FlatBuffers answers, by key) and the site's counts. A query that took a few hundred
 * microseconds of PostgreSQL each time is then a lookup; on two cores the database was what a load of reads ran
 * out of first.
 *
 * Invalidate() drops it all. BlogWorld calls it where the prerendered pages go stale too
 * (RequestRebuild: a post, tag or the site changed or was published at its time), on a
 * comment's change (the counts the posts show, the comments' lists; an edit of its text too), on an
 * account's (an author's or a commenter's name, picture, role) and every Cleanup (what a stream of
 * made up cursors filled it with goes too). Only what worked is kept: a missing post, a redirect, an
 * error are asked again.
 *
 * Main thread only. A worker's answer is kept only if no Invalidate() came since its query started
 * (GetGeneration before it, Store with it): its rows may be older than the change.
 */

#ifndef BLOG_READ_CACHE_H
#define BLOG_READ_CACHE_H

#include "BlogTypes.h"
#include <string>
#include <unordered_map>

namespace Blog
{
class ReadCache
{
public:
    // What the kept answers may take in all; past it nothing more is kept until the next Invalidate
    static constexpr std::size_t MaxBytes = 32 * 1024 * 1024;

    uint64 GetGeneration() const { return _generation; }

    std::string const* Find(std::string const& key) const
    {
        auto itr = _answers.find(key);
        return itr != _answers.end() ? &itr->second : nullptr;
    }

    void Store(std::string key, std::string const& payload, uint64 generation)
    {
        if (generation != _generation || _bytes + key.size() + payload.size() > MaxBytes)
            return;
        _bytes += key.size() + payload.size();
        _answers.insert_or_assign(std::move(key), payload);
    }

    Optional<SiteCounts> const& GetSiteCounts() const { return _siteCounts; }
    void StoreSiteCounts(SiteCounts counts, uint64 generation)
    {
        if (generation == _generation)
            _siteCounts = counts;
    }

    void Invalidate()
    {
        ++_generation;
        _answers.clear();
        _bytes = 0;
        _siteCounts.reset();
    }

    std::size_t GetCount() const { return _answers.size(); }
    std::size_t GetBytes() const { return _bytes; }

private:
    uint64 _generation = 0;
    std::unordered_map<std::string, std::string> _answers;
    std::size_t _bytes = 0;
    Optional<SiteCounts> _siteCounts;
};
}

#endif // BLOG_READ_CACHE_H
