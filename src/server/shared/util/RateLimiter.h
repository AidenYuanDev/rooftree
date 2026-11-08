/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * "At most MaxCount per Window" for any key, on one thread (the main thread). AntiDOS limits each
 * session; this is for limits by something else that the app only knows once a request runs: per
 * account (comments, whoever's address they come from), per email address (password reset mails),
 * per IP across sessions.
 *
 * The window slides: the timestamps of the allowed events are kept, at most MaxCount per key, and
 * Cleanup() drops keys whose events all left the window.
 */

#ifndef ROOFTREE_RATE_LIMITER_H
#define ROOFTREE_RATE_LIMITER_H

#include "Duration.h"
#include <deque>
#include <unordered_map>

namespace Rooftree::Server
{
template<typename Key, typename Hash = std::hash<Key>>
class RateLimiter
{
public:
    RateLimiter(uint32 maxCount, Milliseconds window) : _maxCount(maxCount), _window(window) { }

    void SetLimit(uint32 maxCount, Milliseconds window)
    {
        _maxCount = maxCount;
        _window = window;
    }

    // Counts the event and returns true if the key is within its limit; false (not counted) otherwise
    bool Allow(Key const& key, TimePoint now = TimePoint::clock::now())
    {
        if (!_maxCount)
            return true;

        std::deque<TimePoint>& events = _events[key];
        Expire(events, now);
        if (events.size() >= _maxCount)
            return false;

        events.push_back(now);
        return true;
    }

    // When the key may have its next event, now if it may already
    TimePoint GetRetryTime(Key const& key, TimePoint now = TimePoint::clock::now())
    {
        auto itr = _events.find(key);
        if (itr == _events.end())
            return now;

        Expire(itr->second, now);
        if (itr->second.size() < _maxCount)
            return now;

        return itr->second.front() + _window;
    }

    // Every now and then, so keys seen once don't stay forever
    void Cleanup(TimePoint now = TimePoint::clock::now())
    {
        std::erase_if(_events, [&](auto& entry)
        {
            Expire(entry.second, now);
            return entry.second.empty();
        });
    }

    std::size_t GetKeyCount() const { return _events.size(); }

private:
    void Expire(std::deque<TimePoint>& events, TimePoint now) const
    {
        while (!events.empty() && now - events.front() >= _window)
            events.pop_front();
    }

    uint32 _maxCount;
    Milliseconds _window;
    std::unordered_map<Key, std::deque<TimePoint>, Hash> _events;
};
}

#endif // ROOFTREE_RATE_LIMITER_H
