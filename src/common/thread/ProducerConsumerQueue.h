/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A queue that a consumer waits on: worker threads that sleep until there is work (a map updater
 * handing each map's update to one of them per world tick). Any number of threads on both ends.
 *
 *   while (queue.WaitAndPop(work))     // false once cancelled: the thread ends
 *       work->Run();
 *
 * Cancel() wakes every waiter and drops what is queued: items that own something should be
 * std::unique_ptr.
 */

#ifndef ROOFTREE_PRODUCER_CONSUMER_QUEUE_H
#define ROOFTREE_PRODUCER_CONSUMER_QUEUE_H

#include <condition_variable>
#include <deque>
#include <mutex>

template<typename T>
class ProducerConsumerQueue
{
public:
    // Does nothing once cancelled
    void Push(T item)
    {
        {
            std::scoped_lock lock(_mutex);
            if (_cancelled)
                return;
            _items.push_back(std::move(item));
        }
        _ready.notify_one();
    }

    // Without waiting; false when empty or cancelled
    bool Pop(T& item)
    {
        std::scoped_lock lock(_mutex);
        return Take(item);
    }

    // Waits for an item; false once cancelled
    bool WaitAndPop(T& item)
    {
        std::unique_lock lock(_mutex);
        _ready.wait(lock, [this] { return !_items.empty() || _cancelled; });
        return Take(item);
    }

    void Cancel()
    {
        {
            std::scoped_lock lock(_mutex);
            _cancelled = true;
            _items.clear();
        }
        _ready.notify_all();
    }

    bool IsCancelled() const
    {
        std::scoped_lock lock(_mutex);
        return _cancelled;
    }

    std::size_t Size() const
    {
        std::scoped_lock lock(_mutex);
        return _items.size();
    }

private:
    // Under the lock
    bool Take(T& item)
    {
        if (_cancelled || _items.empty())
            return false;
        item = std::move(_items.front());
        _items.pop_front();
        return true;
    }

    mutable std::mutex _mutex;
    std::condition_variable _ready;
    std::deque<T> _items;
    bool _cancelled = false;
};

#endif // ROOFTREE_PRODUCER_CONSUMER_QUEUE_H
