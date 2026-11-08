/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Player.h"
#include "PreparedStatement.h"
#include "Transaction.h"
#include <algorithm>
#include <cmath>

Player::Player(uint32 id, std::string name, uint32 totalCoins, uint32 playTimeSeconds)
    : _id(id), _name(std::move(name)), _totalCoins(totalCoins), _playTimeMs(playTimeSeconds * 1000)
{
}

void Player::Update(uint32 diff, float speed, float worldWidth, float worldHeight)
{
    _playTimeMs += diff;

    if (!_inputX && !_inputY)
        return;

    // Diagonal movement is not faster
    float length = std::sqrt(float(_inputX * _inputX + _inputY * _inputY));
    float distance = speed * float(diff) / 1000.0f;
    _x = std::clamp(_x + _inputX / length * distance, Radius, worldWidth - Radius);
    _y = std::clamp(_y + _inputY / length * distance, Radius, worldHeight - Radius);
}

void Player::AddCoins(uint32 coins)
{
    _score += coins;
    _totalCoins += coins;
    _saveNeeded = true;
}

void Player::SaveToDB(ArenaDatabaseTransaction& trans)
{
    ArenaDatabasePreparedStatement* stmt = ArenaDatabase.GetPreparedStatement(ARENA_UPD_PLAYER);
    stmt->setUInt32(0, _totalCoins);
    stmt->setUInt32(1, _playTimeMs / 1000);
    stmt->setUInt32(2, _id);
    trans->Append(stmt);

    _saveNeeded = false;
}
