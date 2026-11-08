/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ArenaDatabase.h"
#include "DatabaseWorkerPool.h"

ArenaDatabaseConnection::ArenaDatabaseConnection(DatabaseConnectionInfo const& info) : DatabaseConnection(info)
{
}

ArenaDatabaseConnection::~ArenaDatabaseConnection() = default;

void ArenaDatabaseConnection::DoPrepareStatements()
{

    PrepareStatement(ARENA_SEL_ACCOUNT_BY_NAME, "SELECT id, username, password_hash FROM account WHERE username = ?");
    PrepareStatement(ARENA_INS_ACCOUNT, "INSERT INTO account (username, password_hash) VALUES (?, ?)");
    PrepareStatement(ARENA_INS_PLAYER, "INSERT INTO player (account_id) SELECT id FROM account WHERE username = ?");
    PrepareStatement(ARENA_UPD_LAST_LOGIN, "UPDATE account SET last_login = now() WHERE id = ?");
    PrepareStatement(ARENA_SEL_PLAYER, "SELECT total_coins, play_time FROM player WHERE account_id = ?");
    // Async while running, direct when everyone is saved at shutdown
    PrepareStatement(ARENA_UPD_PLAYER, "UPDATE player SET total_coins = ?, play_time = ? WHERE account_id = ?");
    PrepareStatement(ARENA_SEL_LEADERBOARD, "SELECT a.username, p.total_coins FROM player p JOIN account a ON a.id = p.account_id "
        "ORDER BY p.total_coins DESC, a.id LIMIT ?");
}

template class DatabaseWorkerPool<ArenaDatabaseConnection>;

ArenaDatabaseWorkerPool ArenaDatabase;
