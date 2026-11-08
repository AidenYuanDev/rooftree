/*
 * rooftree-database against a real PostgreSQL server: sync query latency and async throughput through
 * DatabaseWorkerPool. Numbers are dominated by the server and the connection, so they are only
 * comparable on the same setup.
 *
 * Skipped unless Database.Test is filled in in build/bin/rooftree_test.yaml (see tests/rooftree_test.yaml.dist
 * and tests/TestDatabaseConfig.h), creates and drops its own table (rooftree_bench) in that database.
 */

#include "rooftree_benchmark.h"

#include "AsyncCallbackProcessor.h"
#include "DatabaseWorkerPool.h"
#include "Field.h"
#include "DatabaseConnection.h"
#include "PreparedStatement.h"
#include "QueryCallback.h"
#include "QueryHolder.h"
#include "QueryResult.h"
#include "TestDatabaseConfig.h"
#include "Transaction.h"
#include <mutex>
#include <optional>
#include <string>

using namespace std::chrono_literals;

enum BenchDatabaseStatements : uint32
{
    BENCH_INS_ROW,
    BENCH_SEL_ROW,
    BENCH_UPD_SCORE,

    MAX_BENCHDATABASE_STATEMENTS
};

class BenchDatabaseConnection : public DatabaseConnection
{
public:
    using Statements = BenchDatabaseStatements;

    BenchDatabaseConnection(DatabaseConnectionInfo const& info) : DatabaseConnection(info) { }

    void DoPrepareStatements() override
    {

        PrepareStatement(BENCH_INS_ROW, "REPLACE INTO rooftree_bench (id, name, score) VALUES (?, ?, ?)");
        PrepareStatement(BENCH_SEL_ROW, "SELECT id, name, score FROM rooftree_bench WHERE id = ?");
        PrepareStatement(BENCH_UPD_SCORE, "UPDATE rooftree_bench SET score = score + 1 WHERE id = ?");
    }
};

// Not in an anonymous namespace: with internal linkage GCC warns about every unused member of the instantiation
template class DatabaseWorkerPool<BenchDatabaseConnection>;

namespace
{
using BenchDatabaseWorkerPool = DatabaseWorkerPool<BenchDatabaseConnection>;
using BenchStatement = PreparedStatement<BenchDatabaseConnection>;

constexpr uint32 RowCount = 1000;

// One async connection keeps async operations in submission order, so waiting for a final query
// also waits for everything queued before it
class DatabaseFixture
{
public:
    DatabaseFixture()
    {
        std::string why;
        std::optional<DatabaseConfig> database = Rooftree::Test::GetTestDatabaseConfig(why);
        if (!database)
            SKIP(why);


        Database.SetConnectionInfo(database->Connection, 1, 1);
        REQUIRE(Database.Open() == 0);
        _open = true;

        Database.DirectExecute("DROP TABLE IF EXISTS rooftree_bench");
        Database.DirectExecute("CREATE TABLE rooftree_bench (id BIGINT NOT NULL PRIMARY KEY, name VARCHAR(32) NOT NULL, score BIGINT NOT NULL)");
        REQUIRE(Database.PrepareStatements());

        SQLTransaction<BenchDatabaseConnection> trans = Database.BeginTransaction();
        for (uint32 id = 1; id <= RowCount; ++id)
            trans->Append(InsertRow(id, id));
        Database.DirectCommitTransaction(trans);
    }

    ~DatabaseFixture()
    {
        if (!_open)
            return;

        Database.DirectExecute("DROP TABLE IF EXISTS rooftree_bench");
        Database.Close();
    }

    BenchStatement* InsertRow(uint32 id, int64 score)
    {
        BenchStatement* stmt = Database.GetPreparedStatement(BENCH_INS_ROW);
        stmt->setUInt32(0, id);
        stmt->setString(1, std::string_view("bench row"));
        stmt->setInt64(2, score);
        return stmt;
    }

    BenchStatement* SelectRow(uint32 id)
    {
        BenchStatement* stmt = Database.GetPreparedStatement(BENCH_SEL_ROW);
        stmt->setUInt32(0, id);
        return stmt;
    }

    // What a world/session Update() loop does every tick, without the sleep
    template <typename Callback>
    static bool PollUntilInvoked(Callback& callback)
    {
        auto end = std::chrono::steady_clock::now() + 10s;
        while (!callback.InvokeIfReady())
        {
            if (std::chrono::steady_clock::now() > end)
                return false;
            std::this_thread::yield();
        }
        return true;
    }

    // Blocks until every async operation queued so far has run
    void WaitForAsyncQueue()
    {
        QueryCallback callback = Database.AsyncQuery(SelectRow(1)).WithPreparedCallback([](PreparedQueryResult) { });
        REQUIRE(PollUntilInvoked(callback));
    }

    BenchDatabaseWorkerPool Database;

private:
    bool _open = false;
};
}

TEST_CASE_METHOD(DatabaseFixture, "Database synchronous queries", "[database]")
{
    uint32 id = 0;

    BENCHMARK("prepared SELECT by primary key")
    {
        id = id % RowCount + 1;
        PreparedQueryResult result = Database.Query(SelectRow(id));
        return result ? result->Fetch()[2].GetInt64() : 0;
    };

    BENCHMARK("ad-hoc SELECT by primary key (PQuery)")
    {
        id = id % RowCount + 1;
        QueryResult result = Database.PQuery("SELECT id, name, score FROM rooftree_bench WHERE id = {}", id);
        return result ? result->Fetch()[2].GetInt64() : 0;
    };

    BENCHMARK("ad-hoc SELECT, 1000 rows fetched")
    {
        QueryResult result = Database.Query("SELECT id, name, score FROM rooftree_bench");
        int64 sum = 0;
        if (result)
        {
            do
                sum += result->Fetch()[2].GetInt64();
            while (result->NextRow());
        }
        return sum;
    };

    BENCHMARK("DirectExecute prepared REPLACE")
    {
        id = id % RowCount + 1;
        Database.DirectExecute(InsertRow(id, id));
        return id;
    };

    BENCHMARK("DirectCommitTransaction of 100 REPLACEs")
    {
        SQLTransaction<BenchDatabaseConnection> trans = Database.BeginTransaction();
        for (uint32 i = 1; i <= 100; ++i)
            trans->Append(InsertRow(i, i));
        Database.DirectCommitTransaction(trans);
        return 100;
    };
}

TEST_CASE_METHOD(DatabaseFixture, "Database asynchronous work", "[database]")
{
    uint32 id = 0;

    BENCHMARK("AsyncQuery round trip (queue, run, callback)")
    {
        id = id % RowCount + 1;
        int64 score = 0;
        QueryCallback callback = Database.AsyncQuery(SelectRow(id)).WithPreparedCallback([&](PreparedQueryResult result)
        {
            if (result)
                score = result->Fetch()[2].GetInt64();
        });
        PollUntilInvoked(callback);
        return score;
    };

    BENCHMARK("100 queued Execute UPDATEs until done")
    {
        for (uint32 i = 1; i <= 100; ++i)
        {
            BenchStatement* stmt = Database.GetPreparedStatement(BENCH_UPD_SCORE);
            stmt->setUInt32(0, i);
            Database.Execute(stmt);
        }
        WaitForAsyncQueue();
        return 100;
    };

    BENCHMARK("AsyncCommitTransaction of 100 REPLACEs")
    {
        SQLTransaction<BenchDatabaseConnection> trans = Database.BeginTransaction();
        for (uint32 i = 1; i <= 100; ++i)
            trans->Append(InsertRow(i, i));
        TransactionCallback callback = Database.AsyncCommitTransaction(trans);
        bool success = false;
        callback.AfterComplete([&](bool ok) { success = ok; });
        PollUntilInvoked(callback);
        return success;
    };

    BENCHMARK("query holder with 10 SELECTs")
    {
        auto holder = std::make_shared<SQLQueryHolder<BenchDatabaseConnection>>();
        holder->SetSize(10);
        for (uint32 i = 0; i < 10; ++i)
            holder->SetPreparedQuery(i, SelectRow(i + 1));

        SQLQueryHolderCallback callback = Database.DelayQueryHolder(holder);
        int found = 0;
        callback.AfterComplete([&](SQLQueryHolderBase const& completed)
        {
            for (uint32 i = 0; i < 10; ++i)
                found += completed.GetPreparedResult(i) ? 1 : 0;
        });
        PollUntilInvoked(callback);
        return found;
    };

    // Callbacks collected by a processor and polled once per "tick", like a session update
    BENCHMARK("100 AsyncQuery through a QueryCallbackProcessor")
    {
        QueryCallbackProcessor processor;
        int done = 0;
        for (uint32 i = 1; i <= 100; ++i)
            processor.AddCallback(Database.AsyncQuery(SelectRow(i)).WithPreparedCallback([&](PreparedQueryResult) { ++done; }));

        auto end = std::chrono::steady_clock::now() + 10s;
        while (done < 100 && std::chrono::steady_clock::now() < end)
            processor.ProcessReadyCallbacks();
        return done;
    };
}
