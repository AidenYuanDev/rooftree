/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * One connection to PostgreSQL (libpq) and its prepared statements, used by one thread at a time; the
 * pool (DatabaseWorkerPool.h) sees to that. An application derives from it to list its statements:
 *
 *   class MyConnection : public DatabaseConnection
 *   {
 *       using Statements = MyStatements;                   // the pool needs this enum
 *       void DoPrepareStatements() override { PrepareStatement(MY_SEL_X, "SELECT x FROM t WHERE id = ?"); }
 *   };
 *
 * Parameters are written "?" and become PostgreSQL's $1, $2... Preparing checks a statement against the
 * tables and its types, so one that can't run fails at startup and not when it is first used. Where
 * nothing says a parameter's type (INSERT ... SELECT ?, or a bare SELECT ?), cast it: ?::bigint.
 *
 * A failing statement is logged with its SQL and message, never its parameters (they hold passwords). A
 * lost connection is opened again with its statements; a statement outside a transaction runs again once.
 */

#ifndef ROOFTREE_DATABASE_CONNECTION_H
#define ROOFTREE_DATABASE_CONNECTION_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;

struct DatabaseConnectionInfo
{
    std::string user;
    std::string password;
    std::string database;
    std::string host;               // "." with a socket's directory in port_or_socket
    std::string port_or_socket;
    std::string ssl;                // "ssl" requires TLS, "disabled" refuses it, empty takes what the server offers
};

class DatabaseConnection
{
public:
    explicit DatabaseConnection(DatabaseConnectionInfo const& info);
    DatabaseConnection(DatabaseConnection const&) = delete;
    DatabaseConnection& operator=(DatabaseConnection const&) = delete;
    virtual ~DatabaseConnection();

    // True when connected
    bool Open();
    void Close();
    bool PrepareStatements();

    bool Execute(char const* sql);
    bool Execute(PreparedStatementBase const* statement);
    QueryResult Query(char const* sql);
    PreparedQueryResult Query(PreparedStatementBase const* statement);
    // Execute of an INSERT, answers the id its sequence gave. 0 if it failed or inserted nothing
    uint64 Insert(PreparedStatementBase const* statement);
    // All of it or nothing, tried again when PostgreSQL gave up on it for a deadlock
    bool ExecuteTransaction(TransactionBase const& transaction);

    std::string EscapeString(std::string_view text);
    void Ping();

    // How many parameters a prepared statement takes
    uint8 GetParameterCount(uint32 index) const;
    std::size_t GetStatementCount() const { return _statements.size(); }

protected:
    void PrepareStatement(uint32 index, std::string_view sql);
    virtual void DoPrepareStatements() = 0;

private:
    struct Statement
    {
        std::string Sql;            // with $1, $2...
        uint8 ParameterCount = 0;
        bool Prepared = false;
    };

    enum class Outcome { Ok, Failed, Retry };

    PGresult* RunOnce(char const* sql);
    PGresult* RunOnce(PreparedStatementBase const* statement);
    template<typename T>
    PGresult* Run(T const& what, bool retry);
    Outcome Check(PGresult*& result, std::string_view what);
    bool Reconnect();
    bool PrepareOne(uint32 index);
    bool RunTransaction(TransactionBase const& transaction, bool& retry);
    std::string_view GetSql(PreparedStatementBase const* statement) const;
    static std::string_view GetSql(char const* sql) { return sql; }

    DatabaseConnectionInfo _info;
    PGconn* _connection = nullptr;
    std::vector<Statement> _statements;
    bool _prepareError = false;
};

#endif // ROOFTREE_DATABASE_CONNECTION_H
