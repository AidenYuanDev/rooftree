/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "QueryCallback.h"
#include <chrono>
#include <utility>

QueryCallback&& QueryCallback::WithPreparedCallback(std::function<void(PreparedQueryResult)>&& callback)
{
    return WithChainingPreparedCallback([callback = std::move(callback)](QueryCallback&, QueryResult result) { callback(std::move(result)); });
}

QueryCallback&& QueryCallback::WithChainingPreparedCallback(std::function<void(QueryCallback&, PreparedQueryResult)>&& callback)
{
    _callbacks.push_back(std::move(callback));
    return std::move(*this);
}

bool QueryCallback::InvokeIfReady()
{
    while (true)
    {
        if (!_result.valid() || _result.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
            return !_result.valid() || _callbacks.empty();

        QueryResult result = _result.get();
        if (_callbacks.empty())
            return true;

        std::function<void(QueryCallback&, QueryResult)> callback = std::move(_callbacks.front());
        _callbacks.pop_front();
        callback(*this, std::move(result));

        // No further query: done, whatever callbacks are left have nothing to work on
        if (!_result.valid())
            return true;
    }
}
