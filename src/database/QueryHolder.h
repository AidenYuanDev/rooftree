/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Several prepared queries that run one after the other on a connection, their results kept by position.
 */

#ifndef ROOFTREE_DATABASE_QUERY_HOLDER_H
#define ROOFTREE_DATABASE_QUERY_HOLDER_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "PreparedStatement.h"
#include <functional>
#include <future>
#include <memory>
#include <vector>

class SQLQueryHolderBase
{
public:
    virtual ~SQLQueryHolderBase();

    void SetSize(std::size_t size);
    PreparedQueryResult GetPreparedResult(std::size_t index) const;
    void Run(DatabaseConnection& connection);

protected:
    bool SetStatement(std::size_t index, PreparedStatementBase* statement);

private:
    struct Entry
    {
        std::unique_ptr<PreparedStatementBase> Statement;
        PreparedQueryResult Result;
    };

    std::vector<Entry> _entries;
};

template<typename T>
class SQLQueryHolder : public SQLQueryHolderBase
{
public:
    bool SetPreparedQuery(std::size_t index, PreparedStatement<T>* statement) { return SetStatement(index, statement); }
};

class SQLQueryHolderCallback
{
public:
    SQLQueryHolderCallback(std::shared_ptr<SQLQueryHolderBase> holder, std::future<void>&& future)
        : _holder(std::move(holder)), _future(std::move(future)) { }

    void AfterComplete(std::function<void(SQLQueryHolderBase const&)> callback) & { _callback = std::move(callback); }
    bool InvokeIfReady();

private:
    std::shared_ptr<SQLQueryHolderBase> _holder;
    std::future<void> _future;
    std::function<void(SQLQueryHolderBase const&)> _callback;
};

inline bool InvokeAsyncCallbackIfReady(SQLQueryHolderCallback& callback) { return callback.InvokeIfReady(); }

#endif // ROOFTREE_DATABASE_QUERY_HOLDER_H
