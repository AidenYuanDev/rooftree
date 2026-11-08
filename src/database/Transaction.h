/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Statements to run together. Append them, then commit through the pool: all of them happen or none.
 */

#ifndef ROOFTREE_DATABASE_TRANSACTION_H
#define ROOFTREE_DATABASE_TRANSACTION_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "PreparedStatement.h"
#include "StringFormat.h"
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

class TransactionBase
{
public:
    using Step = std::variant<std::unique_ptr<PreparedStatementBase>, std::string>;

    virtual ~TransactionBase() = default;

    void Append(char const* sql) { _steps.emplace_back(std::string(sql)); }

    template<typename... Args>
    void PAppend(Rooftree::FormatString<Args...> sql, Args&&... args)
    {
        Append(Rooftree::StringFormat(sql, std::forward<Args>(args)...).c_str());
    }

    std::size_t GetSize() const { return _steps.size(); }
    std::vector<Step> const& GetSteps() const { return _steps; }

protected:
    void AppendStatement(PreparedStatementBase* statement) { _steps.emplace_back(std::unique_ptr<PreparedStatementBase>(statement)); }

private:
    std::vector<Step> _steps;
};

template<typename T>
class Transaction : public TransactionBase
{
public:
    using TransactionBase::Append;

    void Append(PreparedStatement<T>* statement) { AppendStatement(statement); }
};

// What an asynchronous commit answers: polled like a QueryCallback, on the thread that polls it
class TransactionCallback
{
public:
    explicit TransactionCallback(std::future<bool>&& future) : _future(std::move(future)) { }

    void AfterComplete(std::function<void(bool)> callback) & { _callback = std::move(callback); }
    bool InvokeIfReady();

private:
    std::future<bool> _future;
    std::function<void(bool)> _callback;
};

inline bool InvokeAsyncCallbackIfReady(TransactionCallback& callback) { return callback.InvokeIfReady(); }

#endif // ROOFTREE_DATABASE_TRANSACTION_H
