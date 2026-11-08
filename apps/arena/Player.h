/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The character of a logged in account, the in-world part of its session.
 * World thread only. Survives a relog: when the same account logs in again, the new session takes it
 * over from the old one instead of loading it again (and racing the old session's save).
 */

#ifndef ARENA_PLAYER_H
#define ARENA_PLAYER_H

#include "ArenaDatabase.h"
#include "Define.h"
#include <string>

class Player
{
public:
    static constexpr float Radius = 16.0f;

    Player(uint32 id, std::string name, uint32 totalCoins, uint32 playTimeSeconds);

    uint32 GetId() const { return _id; }
    std::string const& GetName() const { return _name; }

    float GetX() const { return _x; }
    float GetY() const { return _y; }
    void Relocate(float x, float y) { _x = x; _y = y; }

    // dx, dy in -1..1, kept until the client sends the next input
    void SetInput(int32 dx, int32 dy) { _inputX = dx; _inputY = dy; }

    // Also counts play time
    void Update(uint32 diff, float speed, float worldWidth, float worldHeight);

    uint32 GetScore() const { return _score; }      // this connection
    uint32 GetTotalCoins() const { return _totalCoins; }
    void AddCoins(uint32 coins);

    bool IsSaveNeeded() const { return _saveNeeded; }
    void SaveToDB(ArenaDatabaseTransaction& trans);

private:
    uint32 _id;
    std::string _name;

    float _x = 0.0f;
    float _y = 0.0f;
    int32 _inputX = 0;
    int32 _inputY = 0;

    uint32 _score = 0;
    uint32 _totalCoins;
    uint32 _playTimeMs;
    bool _saveNeeded = false;
};

#endif // ARENA_PLAYER_H
