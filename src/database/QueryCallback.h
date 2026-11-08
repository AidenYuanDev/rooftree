/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A query running on another thread and what to do with its answer, on the thread that polls this
 * (a session's update). The callbacks run in the order they were added; one that calls SetNextQuery()
 * hands the next callback that query's answer instead of waiting no more.
 */

#ifndef ROOFTREE_DATABASE_QUERY_CALLBACK_H
#define ROOFTREE_DATABASE_QUERY_CALLBACK_H

#include "AsyncCallbackProcessorFwd.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <deque>
#include <functional>
#include <future>

class QueryCallback
{
public:
    explicit QueryCallback(QueryResultFuture&& result) : _result(std::move(result)) { }
    QueryCallback(QueryCallback&&) noexcept = default;
    QueryCallback& operator=(QueryCallback&&) noexcept = default;

    QueryCallback&& WithPreparedCallback(std::function<void(PreparedQueryResult)>&& callback);
    QueryCallback&& WithChainingPreparedCallback(std::function<void(QueryCallback&, PreparedQueryResult)>&& callback);
    void SetNextQuery(QueryCallback&& next) { _result = std::move(next._result); }

    // True when it is done
    bool InvokeIfReady();

private:
    QueryResultFuture _result;
    std::deque<std::function<void(QueryCallback&, QueryResult)>> _callbacks;
};

inline bool InvokeAsyncCallbackIfReady(QueryCallback& callback) { return callback.InvokeIfReady(); }

using QueryCallbackProcessor = AsyncCallbackProcessor<QueryCallback>;

#endif // ROOFTREE_DATABASE_QUERY_CALLBACK_H
