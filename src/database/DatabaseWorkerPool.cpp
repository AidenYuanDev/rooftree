/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "DatabaseWorkerPool.h"
#include "Log.h"
#include "QueryResult.h"
#include <algorithm>

DatabasePool::DatabasePool(Factory factory) : _factory(std::move(factory))
{
}

DatabasePool::~DatabasePool() = default;

void DatabasePool::SetConnectionInfo(DatabaseConnectionInfo const& info, uint8 asyncThreads, uint8 synchThreads)
{
    _info = std::make_unique<DatabaseConnectionInfo>(info);
    _asyncThreads = std::max<uint8>(asyncThreads, 1);
    _synchThreads = std::max<uint8>(synchThreads, 1);
}

uint32 DatabasePool::Open()
{
    ASSERT(_info, "SetConnectionInfo before Open");
    uint32 failures = 0;
    auto open = [&](std::vector<std::unique_ptr<DatabaseConnection>>& connections, uint8 count)
    {
        for (uint8 i = 0; i < count; ++i)
        {
            std::unique_ptr<DatabaseConnection> connection = _factory(*_info);
            if (connection->Open())
                connections.push_back(std::move(connection));
            else
                ++failures;
        }
    };

    open(_asyncConnections, _asyncThreads);
    open(_synchConnections, _synchThreads);
    if (failures)
    {
        _asyncConnections.clear();
        _synchConnections.clear();
        return failures;
    }

    for (std::unique_ptr<DatabaseConnection> const& connection : _synchConnections)
        _free.push_back(connection.get());

    _stopping = false;
    for (std::unique_ptr<DatabaseConnection> const& connection : _asyncConnections)
        _workers.emplace_back([this, connection = connection.get()] { WorkerLoop(*connection); });

    RT_LOG_INFO("sql.driver", "Database {}: {} asynchronous and {} synchronous connections", _info->database, _asyncThreads, _synchThreads);
    return 0;
}

void DatabasePool::Close()
{
    {
        std::scoped_lock lock(_queueLock);
        _stopping = true;
    }
    _queueReady.notify_all();

    // What is queued still runs
    for (std::thread& worker : _workers)
        worker.join();
    _workers.clear();

    _free.clear();
    _asyncConnections.clear();
    _synchConnections.clear();
}

bool DatabasePool::PrepareStatements()
{
    // Each on the thread that owns its connection: nothing may be queued before, it would need these
    std::vector<std::future<bool>> results;
    for (std::size_t i = 0; i < _asyncConnections.size(); ++i)
    {
        auto promise = std::make_shared<std::promise<bool>>();
        results.push_back(promise->get_future());
        Post([promise](DatabaseConnection& connection) { promise->set_value(connection.PrepareStatements()); });
    }

    bool ok = true;
    for (std::unique_ptr<DatabaseConnection> const& connection : _synchConnections)
        ok = connection->PrepareStatements() && ok;
    for (std::future<bool>& result : results)
        ok = result.get() && ok;
    return ok;
}

uint8 DatabasePool::GetParameterCount(uint32 index) const
{
    ASSERT(!_synchConnections.empty(), "The pool isn't open");
    return _synchConnections.front()->GetParameterCount(index);
}

void DatabasePool::WorkerLoop(DatabaseConnection& connection)
{
    while (true)
    {
        std::function<void(DatabaseConnection&)> work;
        {
            std::unique_lock lock(_queueLock);
            _queueReady.wait(lock, [this] { return _stopping || !_queue.empty(); });
            if (_queue.empty())
                return;

            work = std::move(_queue.front());
            _queue.pop_front();
            --_queueSize;
        }

        work(connection);
    }
}

void DatabasePool::Post(std::function<void(DatabaseConnection&)> work)
{
    {
        std::scoped_lock lock(_queueLock);
        _queue.push_back(std::move(work));
        ++_queueSize;
    }
    _queueReady.notify_one();
}

DatabasePool::Lease::Lease(DatabasePool& pool) : _pool(pool)
{
    std::unique_lock lock(_pool._freeLock);
    _pool._freeReady.wait(lock, [this] { return !_pool._free.empty(); });
    _connection = _pool._free.back();
    _pool._free.pop_back();
}

DatabasePool::Lease::~Lease()
{
    {
        std::scoped_lock lock(_pool._freeLock);
        _pool._free.push_back(_connection);
    }
    _pool._freeReady.notify_one();
}

void DatabasePool::Execute(char const* sql)
{
    Post([sql = std::string(sql)](DatabaseConnection& connection) { connection.Execute(sql.c_str()); });
}

void DatabasePool::DirectExecute(char const* sql)
{
    Lease(*this)->Execute(sql);
}

QueryResult DatabasePool::Query(char const* sql)
{
    return Lease(*this)->Query(sql);
}

QueryCallback DatabasePool::AsyncQuery(char const* sql)
{
    auto promise = std::make_shared<QueryResultPromise>();
    QueryResultFuture future = promise->get_future();
    Post([sql = std::string(sql), promise](DatabaseConnection& connection) { promise->set_value(connection.Query(sql.c_str())); });
    return QueryCallback(std::move(future));
}

void DatabasePool::EscapeString(std::string& text)
{
    text = Lease(*this)->EscapeString(text);
}

void DatabasePool::KeepAlive()
{
    for (std::size_t i = 0; i < _synchConnections.size(); ++i)
        Lease(*this)->Ping();

    for (std::size_t i = 0; i < _asyncConnections.size(); ++i)
        Post([](DatabaseConnection& connection) { connection.Ping(); });
}

void DatabasePool::ExecuteDirect(PreparedStatementBase* statement)
{
    std::unique_ptr<PreparedStatementBase> owned(statement);
    Lease(*this)->Execute(statement);
}

uint64 DatabasePool::InsertDirect(PreparedStatementBase* statement)
{
    std::unique_ptr<PreparedStatementBase> owned(statement);
    return Lease(*this)->Insert(statement);
}

PreparedQueryResult DatabasePool::QueryDirect(PreparedStatementBase* statement)
{
    std::unique_ptr<PreparedStatementBase> owned(statement);
    return Lease(*this)->Query(statement);
}

void DatabasePool::ExecuteQueued(PreparedStatementBase* statement)
{
    Post([statement = std::shared_ptr<PreparedStatementBase>(statement)](DatabaseConnection& connection) { connection.Execute(statement.get()); });
}

QueryCallback DatabasePool::QueryQueued(PreparedStatementBase* statement)
{
    auto promise = std::make_shared<QueryResultPromise>();
    QueryResultFuture future = promise->get_future();
    Post([statement = std::shared_ptr<PreparedStatementBase>(statement), promise](DatabaseConnection& connection)
    {
        promise->set_value(connection.Query(statement.get()));
    });
    return QueryCallback(std::move(future));
}

void DatabasePool::CommitQueued(std::shared_ptr<TransactionBase> transaction)
{
    Post([transaction = std::move(transaction)](DatabaseConnection& connection) { connection.ExecuteTransaction(*transaction); });
}

TransactionCallback DatabasePool::CommitAsync(std::shared_ptr<TransactionBase> transaction)
{
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> future = promise->get_future();
    Post([transaction = std::move(transaction), promise](DatabaseConnection& connection) { promise->set_value(connection.ExecuteTransaction(*transaction)); });
    return TransactionCallback(std::move(future));
}

bool DatabasePool::CommitDirect(TransactionBase const& transaction)
{
    return Lease(*this)->ExecuteTransaction(transaction);
}

SQLQueryHolderCallback DatabasePool::RunHolder(std::shared_ptr<SQLQueryHolderBase> holder)
{
    auto promise = std::make_shared<std::promise<void>>();
    std::future<void> future = promise->get_future();
    Post([holder, promise](DatabaseConnection& connection)
    {
        holder->Run(connection);
        promise->set_value();
    });
    return SQLQueryHolderCallback(std::move(holder), std::move(future));
}
