/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "DatabaseConnection.h"
#include "Errors.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "Transaction.h"
#include <libpq-fe.h>
#include <fmt/format.h>
#include <array>
#include <charconv>
#include <cstring>
#include <thread>

namespace
{
constexpr int MinServerVersion = 140000;
constexpr int MaxTransactionAttempts = 5;

// "?" outside of quotes, in order, to $1, $2...
std::string NumberPlaceholders(std::string_view sql, uint8& count)
{
    std::string result;
    result.reserve(sql.size() + 16);
    char quote = 0;
    uint32 parameters = 0;
    for (char c : sql)
    {
        if (quote)
        {
            if (c == quote)
                quote = 0;
            result += c;
        }
        else if (c == '\'' || c == '"')
        {
            quote = c;
            result += c;
        }
        else if (c == '?')
        {
            result += '$';
            result += std::to_string(++parameters);
        }
        else
            result += c;
    }

    ASSERT(parameters <= 255, "A statement with %u parameters", parameters);
    count = uint8(parameters);
    return result;
}

std::string StatementName(uint32 index)
{
    return "s" + std::to_string(index);
}

// A deadlock or a serialization failure: the transaction may simply run again
bool IsRetryableState(char const* sqlState)
{
    return sqlState && (std::strcmp(sqlState, "40P01") == 0 || std::strcmp(sqlState, "40001") == 0);
}

void LogNotice(void* /*arg*/, char const* message)
{
    RT_LOG_DEBUG("sql.driver", "{}", message);
}
}

DatabaseConnection::DatabaseConnection(DatabaseConnectionInfo const& info) : _info(info)
{
}

DatabaseConnection::~DatabaseConnection()
{
    Close();
}

bool DatabaseConnection::Open()
{
    bool const socket = _info.host == ".";
    char const* const sslMode = _info.ssl == "ssl" ? "require" : _info.ssl == "disabled" ? "disable" : "prefer";
    std::array keywords{ "host", "port", "user", "password", "dbname", "sslmode", "application_name", "client_encoding", "options", "connect_timeout", static_cast<char const*>(nullptr) };
    std::array values{ socket ? _info.port_or_socket.c_str() : _info.host.c_str(), socket ? "" : _info.port_or_socket.c_str(), _info.user.c_str(), _info.password.c_str(),
        _info.database.c_str(), sslMode, "rooftree", "UTF8", "-c TimeZone=UTC", "10", static_cast<char const*>(nullptr) };

    _connection = PQconnectdbParams(keywords.data(), values.data(), 0);
    if (!_connection || PQstatus(_connection) != CONNECTION_OK)
    {
        RT_LOG_ERROR("sql.driver", "Could not connect to database {} at {}: {}", _info.database, _info.host, _connection ? PQerrorMessage(_connection) : "out of memory");
        Close();
        return false;
    }

    if (PQserverVersion(_connection) < MinServerVersion)
    {
        RT_LOG_ERROR("sql.driver", "PostgreSQL {} is too old, Rooftree needs {} or newer", PQserverVersion(_connection), MinServerVersion);
        Close();
        return false;
    }

    PQsetNoticeProcessor(_connection, &LogNotice, nullptr);
    RT_LOG_DEBUG("sql.driver", "Connected to database {} at {}, PostgreSQL {}", _info.database, _info.host, PQserverVersion(_connection));
    return true;
}

void DatabaseConnection::Close()
{
    if (_connection)
    {
        PQfinish(_connection);
        _connection = nullptr;
    }
}

bool DatabaseConnection::PrepareStatements()
{
    _prepareError = false;
    DoPrepareStatements();
    return !_prepareError;
}

void DatabaseConnection::PrepareStatement(uint32 index, std::string_view sql)
{
    if (index >= _statements.size())
        _statements.resize(index + 1);

    Statement& statement = _statements[index];
    statement.Sql = NumberPlaceholders(sql, statement.ParameterCount);
    if (!PrepareOne(index))
        _prepareError = true;
}

bool DatabaseConnection::PrepareOne(uint32 index)
{
    Statement& statement = _statements[index];
    statement.Prepared = false;
    PGresult* result = PQprepare(_connection, StatementName(index).c_str(), statement.Sql.c_str(), 0, nullptr);
    bool const ok = PQresultStatus(result) == PGRES_COMMAND_OK;
    if (ok)
        statement.Prepared = true;
    else
        RT_LOG_ERROR("sql.sql", "Could not prepare statement {}: {}\nSQL: {}", index, PQresultErrorMessage(result), statement.Sql);
    PQclear(result);
    return ok;
}

uint8 DatabaseConnection::GetParameterCount(uint32 index) const
{
    ASSERT(index < _statements.size() && _statements[index].Prepared, "Statement %u wasn't prepared", index);
    return _statements[index].ParameterCount;
}

std::string_view DatabaseConnection::GetSql(PreparedStatementBase const* statement) const
{
    return _statements[statement->GetIndex()].Sql;
}

PGresult* DatabaseConnection::RunOnce(char const* sql)
{
    return PQexec(_connection, sql);
}

PGresult* DatabaseConnection::RunOnce(PreparedStatementBase const* statement)
{
    uint32 const index = statement->GetIndex();
    ASSERT(index < _statements.size() && _statements[index].Prepared, "Statement %u wasn't prepared", index);

    std::vector<PreparedStatementBase::Parameter> const& parameters = statement->GetParameters();
    std::vector<char const*> values(parameters.size());
    std::vector<int> lengths(parameters.size());
    std::vector<int> formats(parameters.size());
    for (std::size_t i = 0; i < parameters.size(); ++i)
    {
        values[i] = parameters[i].Value ? parameters[i].Value->data() : nullptr;
        lengths[i] = parameters[i].Value ? int(parameters[i].Value->size()) : 0;
        formats[i] = parameters[i].IsBinary ? 1 : 0;
    }

    return PQexecPrepared(_connection, StatementName(index).c_str(), int(parameters.size()), values.data(), lengths.data(), formats.data(), 0);
}

DatabaseConnection::Outcome DatabaseConnection::Check(PGresult*& result, std::string_view what)
{
    ExecStatusType const status = result ? PQresultStatus(result) : PGRES_FATAL_ERROR;
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK)
        return Outcome::Ok;

    bool const lost = PQstatus(_connection) != CONNECTION_OK;
    bool const retryable = result && IsRetryableState(PQresultErrorField(result, PG_DIAG_SQLSTATE));
    if (!lost && !retryable)
        RT_LOG_ERROR("sql.sql", "{}\nSQL: {}", result ? PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY) : PQerrorMessage(_connection), what);

    PQclear(result);
    result = nullptr;
    return lost || retryable ? Outcome::Retry : Outcome::Failed;
}

template<typename T>
PGresult* DatabaseConnection::Run(T const& what, bool retry)
{
    if (!_connection && !Reconnect())
        return nullptr;

    PGresult* result = RunOnce(what);
    switch (Check(result, GetSql(what)))
    {
        case Outcome::Ok:
            return result;
        case Outcome::Failed:
            return nullptr;
        case Outcome::Retry:
            break;
    }

    if (!retry)
        return nullptr;

    if (PQstatus(_connection) != CONNECTION_OK && !Reconnect())
        return nullptr;

    result = RunOnce(what);
    return Check(result, GetSql(what)) == Outcome::Ok ? result : nullptr;
}

bool DatabaseConnection::Reconnect()
{
    RT_LOG_WARN("sql.driver", "Connection to database {} lost, connecting again", _info.database);
    Close();
    if (!Open())
        return false;

    for (uint32 i = 0; i < _statements.size(); ++i)
        if (_statements[i].Prepared)
            PrepareOne(i);
    return true;
}

bool DatabaseConnection::Execute(char const* sql)
{
    PGresult* result = Run(sql, true);
    PQclear(result);
    return result != nullptr;
}

bool DatabaseConnection::Execute(PreparedStatementBase const* statement)
{
    PGresult* result = Run(statement, true);
    PQclear(result);
    return result != nullptr;
}

QueryResult DatabaseConnection::Query(char const* sql)
{
    PGresult* result = Run(sql, true);
    if (!result)
        return nullptr;

    if (PQntuples(result) == 0)
    {
        PQclear(result);
        return nullptr;
    }

    return std::make_shared<ResultSet>(result);
}

PreparedQueryResult DatabaseConnection::Query(PreparedStatementBase const* statement)
{
    PGresult* result = Run(statement, true);
    if (!result)
        return nullptr;

    if (PQntuples(result) == 0)
    {
        PQclear(result);
        return nullptr;
    }

    return std::make_shared<ResultSet>(result);
}

uint64 DatabaseConnection::Insert(PreparedStatementBase const* statement)
{
    PGresult* result = Run(statement, true);
    if (!result)
        return 0;

    bool const inserted = std::strcmp(PQcmdTuples(result), "0") != 0;
    PQclear(result);
    if (!inserted)
        return 0;

    // Per connection, and this one has done nothing else since
    QueryResult id = Query("SELECT lastval()");
    return id ? id->Fetch()[0].GetUInt64() : 0;
}

bool DatabaseConnection::RunTransaction(TransactionBase const& transaction, bool& retry)
{
    retry = false;
    if (!_connection && !Reconnect())
        return false;

    PGresult* result = PQexec(_connection, "BEGIN");
    if (Check(result, "BEGIN") != Outcome::Ok)
    {
        retry = !_connection || PQstatus(_connection) != CONNECTION_OK;
        return false;
    }
    PQclear(result);

    for (TransactionBase::Step const& step : transaction.GetSteps())
    {
        PGresult* stepResult;
        std::string_view sql;
        if (auto const* statement = std::get_if<std::unique_ptr<PreparedStatementBase>>(&step))
        {
            stepResult = RunOnce(statement->get());
            sql = GetSql(statement->get());
        }
        else
        {
            std::string const& text = std::get<std::string>(step);
            stepResult = RunOnce(text.c_str());
            sql = text;
        }

        Outcome const outcome = Check(stepResult, sql);
        if (outcome == Outcome::Ok)
        {
            PQclear(stepResult);
            continue;
        }

        retry = outcome == Outcome::Retry;
        if (PQstatus(_connection) == CONNECTION_OK)
            PQclear(PQexec(_connection, "ROLLBACK"));
        return false;
    }

    result = PQexec(_connection, "COMMIT");
    Outcome const outcome = Check(result, "COMMIT");
    PQclear(result);
    retry = outcome == Outcome::Retry;
    return outcome == Outcome::Ok;
}

bool DatabaseConnection::ExecuteTransaction(TransactionBase const& transaction)
{
    for (int attempt = 1; attempt <= MaxTransactionAttempts; ++attempt)
    {
        bool retry = false;
        if (RunTransaction(transaction, retry))
            return true;

        if (!retry)
            return false;

        if (PQstatus(_connection) != CONNECTION_OK && !Reconnect())
            return false;

        RT_LOG_WARN("sql.sql", "Transaction failed for a deadlock or a lost connection, attempt {} of {}", attempt, MaxTransactionAttempts);
        std::this_thread::sleep_for(std::chrono::milliseconds(10 * attempt));
    }

    RT_LOG_ERROR("sql.sql", "Transaction gave up after {} attempts", MaxTransactionAttempts);
    return false;
}

std::string DatabaseConnection::EscapeString(std::string_view text)
{
    std::string result(text.size() * 2 + 1, '\0');
    int error = 0;
    std::size_t const length = PQescapeStringConn(_connection, result.data(), text.data(), text.size(), &error);
    if (error)
        RT_LOG_ERROR("sql.sql", "Could not escape a string: {}", PQerrorMessage(_connection));
    result.resize(length);
    return result;
}

void DatabaseConnection::Ping()
{
    Execute("SELECT 1");
}
