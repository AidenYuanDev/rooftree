/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The connections of one database and how the application's threads share them:
 *  - Query, DirectExecute, DirectCommitTransaction block the calling thread on a free connection of the
 *    synchronous ones. That is what worker threads do; the main thread shouldn't
 *  - Execute, AsyncQuery, CommitTransaction, DelayQueryHolder go into a queue served by the asynchronous
 *    connections, each with its thread. With one of them they run in the order queued
 *
 * Open, then create what the statements need, then PrepareStatements. A statement goes to the call it is
 * passed to, which destroys it; a transaction owns the ones appended to it.
 */

#ifndef ROOFTREE_DATABASE_WORKER_POOL_H
#define ROOFTREE_DATABASE_WORKER_POOL_H

#include "DatabaseConnection.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Errors.h"
#include "PreparedStatement.h"
#include "QueryCallback.h"
#include "QueryHolder.h"
#include "StringFormat.h"
#include "Transaction.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class DatabasePool
{
public:
    DatabasePool(DatabasePool const&) = delete;
    DatabasePool& operator=(DatabasePool const&) = delete;

    void SetConnectionInfo(DatabaseConnectionInfo const& info, uint8 asyncThreads, uint8 synchThreads);
    // 0 when every connection is open
    uint32 Open();
    void Close();
    bool PrepareStatements();
    DatabaseConnectionInfo const* GetConnectionInfo() const { return _info.get(); }

    void Execute(char const* sql);
    void DirectExecute(char const* sql);
    QueryResult Query(char const* sql);
    QueryCallback AsyncQuery(char const* sql);

    template<typename... Args>
    void PExecute(Rooftree::FormatString<Args...> sql, Args&&... args) { Execute(Rooftree::StringFormat(sql, std::forward<Args>(args)...).c_str()); }
    template<typename... Args>
    void DirectPExecute(Rooftree::FormatString<Args...> sql, Args&&... args) { DirectExecute(Rooftree::StringFormat(sql, std::forward<Args>(args)...).c_str()); }
    template<typename... Args>
    QueryResult PQuery(Rooftree::FormatString<Args...> sql, Args&&... args) { return Query(Rooftree::StringFormat(sql, std::forward<Args>(args)...).c_str()); }

    void EscapeString(std::string& text);
    // A query on each connection, so a server or a firewall in between doesn't take idle ones for dead
    void KeepAlive();
    std::size_t QueueSize() const { return _queueSize; }

protected:
    using Factory = std::function<std::unique_ptr<DatabaseConnection>(DatabaseConnectionInfo const&)>;

    explicit DatabasePool(Factory factory);
    ~DatabasePool();

    void Post(std::function<void(DatabaseConnection&)> work);

    // A synchronous connection for as long as this lives
    class Lease
    {
    public:
        explicit Lease(DatabasePool& pool);
        Lease(Lease const&) = delete;
        Lease& operator=(Lease const&) = delete;
        ~Lease();
        DatabaseConnection* operator->() const { return _connection; }

    private:
        DatabasePool& _pool;
        DatabaseConnection* _connection;
    };

    uint8 GetParameterCount(uint32 index) const;

    void ExecuteDirect(PreparedStatementBase* statement);
    uint64 InsertDirect(PreparedStatementBase* statement);
    PreparedQueryResult QueryDirect(PreparedStatementBase* statement);
    void ExecuteQueued(PreparedStatementBase* statement);
    QueryCallback QueryQueued(PreparedStatementBase* statement);
    void CommitQueued(std::shared_ptr<TransactionBase> transaction);
    TransactionCallback CommitAsync(std::shared_ptr<TransactionBase> transaction);
    bool CommitDirect(TransactionBase const& transaction);
    SQLQueryHolderCallback RunHolder(std::shared_ptr<SQLQueryHolderBase> holder);

private:
    void WorkerLoop(DatabaseConnection& connection);

    Factory _factory;
    std::unique_ptr<DatabaseConnectionInfo> _info;
    uint8 _asyncThreads = 1;
    uint8 _synchThreads = 1;

    std::vector<std::unique_ptr<DatabaseConnection>> _asyncConnections;
    std::vector<std::thread> _workers;
    std::mutex _queueLock;
    std::condition_variable _queueReady;
    std::deque<std::function<void(DatabaseConnection&)>> _queue;
    std::atomic<std::size_t> _queueSize = 0;
    bool _stopping = false;

    std::vector<std::unique_ptr<DatabaseConnection>> _synchConnections;
    std::mutex _freeLock;
    std::condition_variable _freeReady;
    std::vector<DatabaseConnection*> _free;
};

template<typename T>
class DatabaseWorkerPool : public DatabasePool
{
public:
    using PreparedStatementIndex = typename T::Statements;

    DatabaseWorkerPool() : DatabasePool([](DatabaseConnectionInfo const& info) { return std::make_unique<T>(info); }) { }
    ~DatabaseWorkerPool() { Close(); }

    using DatabasePool::Execute;
    using DatabasePool::DirectExecute;
    using DatabasePool::Query;
    using DatabasePool::AsyncQuery;

    PreparedStatement<T>* GetPreparedStatement(PreparedStatementIndex index) { return new PreparedStatement<T>(uint32(index), GetParameterCount(uint32(index))); }

    void Execute(PreparedStatement<T>* statement) { ExecuteQueued(statement); }
    void DirectExecute(PreparedStatement<T>* statement) { ExecuteDirect(statement); }
    uint64 DirectInsert(PreparedStatement<T>* statement) { return InsertDirect(statement); }
    PreparedQueryResult Query(PreparedStatement<T>* statement) { return QueryDirect(statement); }
    QueryCallback AsyncQuery(PreparedStatement<T>* statement) { return QueryQueued(statement); }

    SQLTransaction<T> BeginTransaction() { return std::make_shared<Transaction<T>>(); }
    void CommitTransaction(SQLTransaction<T> transaction) { CommitQueued(std::move(transaction)); }
    TransactionCallback AsyncCommitTransaction(SQLTransaction<T> transaction) { return CommitAsync(std::move(transaction)); }
    void DirectCommitTransaction(SQLTransaction<T>& transaction) { CommitDirect(*transaction); }

    SQLQueryHolderCallback DelayQueryHolder(std::shared_ptr<SQLQueryHolder<T>> holder) { return RunHolder(std::move(holder)); }
};

#endif // ROOFTREE_DATABASE_WORKER_POOL_H
