/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "QueryHolder.h"
#include "DatabaseConnection.h"
#include "Log.h"
#include "QueryResult.h"
#include <chrono>

SQLQueryHolderBase::~SQLQueryHolderBase() = default;

void SQLQueryHolderBase::SetSize(std::size_t size)
{
    _entries.resize(size);
}

bool SQLQueryHolderBase::SetStatement(std::size_t index, PreparedStatementBase* statement)
{
    if (index >= _entries.size())
    {
        RT_LOG_ERROR("sql.sql", "Query holder of {} statements given one at {}", _entries.size(), index);
        delete statement;
        return false;
    }

    _entries[index].Statement.reset(statement);
    return true;
}

PreparedQueryResult SQLQueryHolderBase::GetPreparedResult(std::size_t index) const
{
    return index < _entries.size() ? _entries[index].Result : PreparedQueryResult();
}

void SQLQueryHolderBase::Run(DatabaseConnection& connection)
{
    for (Entry& entry : _entries)
        if (entry.Statement)
            entry.Result = connection.Query(entry.Statement.get());
}

bool SQLQueryHolderCallback::InvokeIfReady()
{
    if (_future.wait_for(std::chrono::seconds::zero()) != std::future_status::ready)
        return false;

    _future.get();
    if (_callback)
        _callback(*_holder);
    return true;
}
