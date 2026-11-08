/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The callbacks of one thread's asynchronous work (a session's database queries), run by that thread
 * when it polls: ProcessReadyCallbacks() once an update runs those whose work finished and keeps the
 * others. A callback may add callbacks while it runs; they are polled from the next call on.
 */

#ifndef ROOFTREE_ASYNC_CALLBACK_PROCESSOR_H
#define ROOFTREE_ASYNC_CALLBACK_PROCESSOR_H

#include "AsyncCallbackProcessorFwd.h"
#include <vector>

template<AsyncCallback T>
class AsyncCallbackProcessor
{
public:
    AsyncCallbackProcessor() = default;
    AsyncCallbackProcessor(AsyncCallbackProcessor const&) = delete;
    AsyncCallbackProcessor& operator=(AsyncCallbackProcessor const&) = delete;

    T& AddCallback(T&& callback) { return _callbacks.emplace_back(std::move(callback)); }

    void ProcessReadyCallbacks()
    {
        // Out of the member first: a callback adding one doesn't move the vector under this loop
        std::vector<T> polled = std::move(_callbacks);
        _callbacks.clear();
        std::erase_if(polled, [](T& callback) { return InvokeAsyncCallbackIfReady(callback); });
        // Those still waiting go first, in the order they were added
        _callbacks.insert(_callbacks.begin(), std::make_move_iterator(polled.begin()), std::make_move_iterator(polled.end()));
    }

    std::size_t Size() const { return _callbacks.size(); }
    bool Empty() const { return _callbacks.empty(); }
    void CancelAll() { _callbacks.clear(); }

private:
    std::vector<T> _callbacks;
};

#endif // ROOFTREE_ASYNC_CALLBACK_PROCESSOR_H
