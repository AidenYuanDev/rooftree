/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#ifndef ROOFTREE_DATABASE_ENV_FWD_H
#define ROOFTREE_DATABASE_ENV_FWD_H

#include <future>
#include <memory>

class Field;
class ResultSet;
using PreparedResultSet = ResultSet;
using QueryResult = std::shared_ptr<ResultSet>;
using PreparedQueryResult = QueryResult;
using QueryResultFuture = std::future<QueryResult>;
using QueryResultPromise = std::promise<QueryResult>;

class DatabaseConnection;
struct DatabaseConnectionInfo;
class PreparedStatementBase;

template<typename T>
class PreparedStatement;

class QueryCallback;
class SQLQueryHolderBase;
class SQLQueryHolderCallback;
class TransactionBase;
class TransactionCallback;

template<typename T>
class SQLQueryHolder;

template<typename T>
class Transaction;

template<typename T>
using SQLTransaction = std::shared_ptr<Transaction<T>>;

#endif // ROOFTREE_DATABASE_ENV_FWD_H
