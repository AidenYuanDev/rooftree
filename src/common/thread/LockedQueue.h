/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A queue under a mutex, for any number of threads on both ends: what a consumer polls once an update
 * (SessionMgr's finished work, the arena's new sessions). next() never waits.
 *
 * For a game whose packets are handled by more than one thread (the map's thread takes a session's
 * movement packets, the world thread the rest), next(item, filter) only takes the front item when
 * filter(item) says yes: each thread polls the same queue with its own filter, and a packet another
 * thread handles waits for it. readd puts items taken but not handled back in front, in their order.
 */

#ifndef ROOFTREE_LOCKED_QUEUE_H
#define ROOFTREE_LOCKED_QUEUE_H

#include <concepts>
#include <deque>
#include <iterator>
#include <utility>
#include <mutex>

template<typename T>
class LockedQueue
{
public:
    void add(T item)
    {
        std::scoped_lock lock(_mutex);
        _items.push_back(std::move(item));
    }

    // In front of what is queued, first to last
    template<std::input_iterator Iterator>
    void readd(Iterator first, Iterator last)
    {
        std::scoped_lock lock(_mutex);
        _items.insert(_items.begin(), first, last);
    }

    // False when it is empty
    bool next(T& item)
    {
        return next(item, [](T const&) { return true; });
    }

    // False when it is empty or filter refuses the front item, which then stays where it is: the
    // order of what one session sent is kept across the threads that take its items
    template<std::predicate<T const&> Filter>
    bool next(T& item, Filter&& filter)
    {
        std::scoped_lock lock(_mutex);
        if (_items.empty() || !filter(std::as_const(_items.front())))
            return false;
        item = std::move(_items.front());
        _items.pop_front();
        return true;
    }

    // Out of date as soon as it returns, unless no one else adds
    bool empty() const
    {
        std::scoped_lock lock(_mutex);
        return _items.empty();
    }

private:
    mutable std::mutex _mutex;
    std::deque<T> _items;
};

#endif // ROOFTREE_LOCKED_QUEUE_H
