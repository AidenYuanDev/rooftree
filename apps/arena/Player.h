/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The character of a logged in account, the in-world part of its session, and a unit
 * of the world's CombatMap (its id is the account's). World thread only. Survives a relog: when the
 * same account logs in again, the new session takes it over from the old one instead of loading it
 * again (and racing the old session's save).
 */

#ifndef ARENA_PLAYER_H
#define ARENA_PLAYER_H

#include "ArenaDatabase.h"
#include "Define.h"
#include "Unit.h"

#include <string>

class Player : public Unit {
 public:
  static constexpr uint32 MaxHealth = 200;
  static constexpr uint32 MaxMana = 100;

  Player(uint32 id, std::string name, uint32 totalCoins, uint32 playTimeSeconds);

  std::string const& GetName() const { return _name; }

  // A direction of any length, kept until the client sends the next input; one too short to tell
  // is standing still. Not moving from now on is not moving for a cast in the same update
  void SetInput(float x, float y);
  // Where the client says the player looks (Unit::GetFacing); until it says, the way it last walked
  void SetClientFacing(float facing);

  // Walks while alive and not stunned, at speed times its speed boosts; also counts play time
  void UpdateMovement(uint32 diff, float speed, float worldWidth, float worldHeight);

  // This connection's
  uint32 GetKills() const { return _kills; }
  uint32 GetDeaths() const { return _deaths; }
  void AddKill() { ++_kills; }
  void AddDeath() { ++_deaths; }

  uint32 GetScore() const { return _score; }  // this connection
  uint32 GetTotalCoins() const { return _totalCoins; }
  void AddCoins(uint32 coins);

  bool IsSaveNeeded() const { return _saveNeeded; }
  void SaveToDB(ArenaDatabaseTransaction& trans);

 private:
  std::string _name;

  bool IsWalking() const { return _inputX != 0.0f || _inputY != 0.0f; }

  float _inputX = 0.0f;  // of length 1, or both 0
  float _inputY = 0.0f;
  bool _facingGiven = false;

  uint32 _score = 0;
  uint32 _kills = 0;
  uint32 _deaths = 0;
  uint32 _totalCoins;
  uint32 _playTimeMs;
  bool _saveNeeded = false;
};

#endif  // ARENA_PLAYER_H
