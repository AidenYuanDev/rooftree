/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Log lines into a table, through an insert statement of the application's connection type that takes
 * (time, realm id, logger name, level, text):
 *
 *   AppenderDB<LoginDatabaseConnection>::SetDatabase(&LoginDatabase, LOGIN_INS_LOG);
 *   sLog->RegisterAppender<AppenderDB<LoginDatabaseConnection>>();
 *   ...
 *   sLog->SetRealmId(realmId);     // it writes nothing before
 *
 * configured like any appender: Appender.DB: "3,2,0" (type 3). The lines of the sql loggers are left
 * out: writing one would log another.
 */

#ifndef ROOFTREE_APPENDER_DB_H
#define ROOFTREE_APPENDER_DB_H

#include "Appender.h"
#include "DatabaseWorkerPool.h"
#include "LogMessage.h"
#include "Optional.h"
#include "PreparedStatement.h"
#include <string_view>
#include <vector>

template<typename Connection>
class AppenderDB : public Appender
{
public:
    static constexpr AppenderType Type = APPENDER_DB;

    AppenderDB(uint8 id, std::string name, LogLevel level, AppenderFlags /*flags*/, std::vector<std::string_view> const& /*args*/)
        : Appender(id, std::move(name), level) { }

    static void SetDatabase(DatabaseWorkerPool<Connection>* database, typename Connection::Statements insert)
    {
        Database = database;
        Insert = insert;
    }

    AppenderType GetType() const override { return Type; }
    void SetRealmId(uint32 realmId) override { _realmId = realmId; }

private:
    void Write(LogMessage const& message, std::string const& /*prefix*/) override
    {
        if (!_realmId || !Database || message.Type.find("sql") != std::string::npos)
            return;

        PreparedStatement<Connection>* stmt = Database->GetPreparedStatement(Insert);
        stmt->setUInt64(0, uint64(message.Time));
        stmt->setUInt32(1, *_realmId);
        stmt->setString(2, message.Type);
        stmt->setUInt8(3, uint8(message.Level));
        stmt->setString(4, message.Text);
        Database->Execute(stmt);
    }

    Optional<uint32> _realmId;

    static inline DatabaseWorkerPool<Connection>* Database = nullptr;
    static inline typename Connection::Statements Insert{};
};

#endif // ROOFTREE_APPENDER_DB_H
