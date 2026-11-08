/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * arena's database: an enum of statements, a DatabaseConnection that
 * prepares them and one global DatabaseWorkerPool.
 *
 * CONNECTION_SYNCH statements run with Query()/DirectExecute() on the HTTP worker threads (register,
 * login), CONNECTION_ASYNC ones are queued by the world thread, which must never block on the database.
 */

#ifndef ARENA_DATABASE_H
#define ARENA_DATABASE_H

#include "DatabaseWorkerPool.h"
#include "DatabaseConnection.h"

enum ArenaDatabaseStatements : uint32
{
    ARENA_SEL_ACCOUNT_BY_NAME,
    ARENA_INS_ACCOUNT,
    ARENA_INS_PLAYER,
    ARENA_UPD_LAST_LOGIN,
    ARENA_SEL_PLAYER,
    ARENA_UPD_PLAYER,
    ARENA_SEL_LEADERBOARD,

    MAX_ARENADATABASE_STATEMENTS
};

class ArenaDatabaseConnection : public DatabaseConnection
{
public:
    using Statements = ArenaDatabaseStatements;

    ArenaDatabaseConnection(DatabaseConnectionInfo const& info);
    ~ArenaDatabaseConnection();

    void DoPrepareStatements() override;
};

using ArenaDatabaseWorkerPool = DatabaseWorkerPool<ArenaDatabaseConnection>;
using ArenaDatabasePreparedStatement = PreparedStatement<ArenaDatabaseConnection>;
using ArenaDatabaseTransaction = SQLTransaction<ArenaDatabaseConnection>;

extern ArenaDatabaseWorkerPool ArenaDatabase;

#endif // ARENA_DATABASE_H
