/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Player.h"
#include "PreparedStatement.h"
#include "Transaction.h"
#include <algorithm>
#include <cmath>

Player::Player(uint32 id, std::string name, uint32 totalCoins, uint32 playTimeSeconds)
    : Unit(id, MaxHealth, MaxMana), _name(std::move(name)), _totalCoins(totalCoins), _playTimeMs(playTimeSeconds * 1000)
{
}

void Player::SetInput(float x, float y)
{
    // Callers pass finite numbers; a length this small is no direction
    float length = std::sqrt(x * x + y * y);
    if (length < 1e-3f)
        _inputX = _inputY = 0.0f;
    else
    {
        _inputX = x / length;
        _inputY = y / length;
    }
    SetMoving(IsWalking() && CanMove());
}

void Player::SetClientFacing(float facing)
{
    SetFacing(facing);
    _facingGiven = true;
}

void Player::UpdateMovement(uint32 diff, float speed, float worldWidth, float worldHeight)
{
    _playTimeMs += diff;

    // Every update: a stun that ended lets a held key walk again
    SetMoving(IsWalking() && CanMove());
    if (!IsMoving())
        return;

    // The direction is of length 1: diagonals are no faster
    float distance = speed * GetSpeedMultiplier() * float(diff) / 1000.0f;
    Relocate(std::clamp(GetX() + _inputX * distance, Radius, worldWidth - Radius),
        std::clamp(GetY() + _inputY * distance, Radius, worldHeight - Radius));
    if (!_facingGiven)
        SetFacing(std::atan2(_inputY, _inputX));
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
    stmt->setUInt32(2, GetId());
    trans->Append(stmt);

    _saveNeeded = false;
}
