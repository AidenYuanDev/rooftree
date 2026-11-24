/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Something that fights: health, power (mana, or energy for a rogue: its class says), a position
 * and facing, the spell it is casting, its cooldowns, combo points and the auras on it. A player
 * and an NPC are both one. World thread only, like everything in the map.
 *
 * Every rule is checked here, the client only asks: CastSpell refuses (CastResult) what the caster
 * can't do now, and a cast that takes time is checked again when it ends, as the target may have
 * died or walked away meanwhile. What happens is told as CombatEvents to the unit's CombatMap,
 * which hands them to whoever sends them on.
 *
 *   start        alive, not casting or stunned, still for a cast that takes time, off cooldown and
 *                global cooldown, enough mana, a living target in range; the global cooldown starts
 *   casting      moving, a stun, death or CancelCast interrupt it
 *   completes    the target is checked again, mana is spent, the cooldown starts, the effects run
 *
 * Apart from spells, a unit may auto attack: StartAttack picks a target, then a swing lands every
 * class's AttackInterval while the target is within MeleeRange, alive, and the attacker is free
 * (not casting, stunned or dead). Out of range the next swing waits and goes the moment it is back
 * in range. The attack stops when its target dies or leaves the map, or the attacker dies.
 */

#ifndef ARENA_UNIT_H
#define ARENA_UNIT_H

#include "Define.h"

#include <vector>

class CombatMap;
struct ClassInfo;
struct CombatEvent;
struct AuraInfo;
struct SpellInfo;
enum class UnitClass : uint8;

// Why CastSpell refused. Arena.fbs's CastError has the same values
enum class CastResult : uint8 {
  Ok,
  UnknownSpell,
  Dead,
  Casting,  // another spell is being cast
  Stunned,
  Moving,    // a spell that takes time is cast standing still
  NotReady,  // its cooldown or the global cooldown
  NoMana,
  NoTarget,  // none given, or no such unit
  TargetDead,
  BadTarget,  // the caster itself for a spell at an enemy
  OutOfRange,
  NoComboPoints  // a finisher with none to spend
};

// Arena.fbs's InterruptReason has the same values
enum class InterruptReason : uint8 {
  None,       // an event that isn't an interrupt
  Cancelled,  // by the caster
  Moved,
  Stunned,
  Died,
  TargetLost,  // dead or gone when the cast ended
  OutOfRange   // when the cast ended
};

struct Aura {
  AuraInfo const* Info;
  uint32 CasterId;
  uint32 Remaining;      // ms
  uint32 TickTimer = 0;  // ms since the last tick
  uint32 Ticks = 0;
};

class Unit {
 public:
  static constexpr float Radius = 16.0f;
  // The auto attack's reach, between the edges, as Strike's; its pace and damage are the class's
  static constexpr float MeleeRange = 5.0f;

  Unit(uint32 id, uint32 maxHealth, uint32 maxMana);
  virtual ~Unit();

  Unit(Unit const&) = delete;
  Unit& operator=(Unit const&) = delete;

  uint32 GetId() const { return _id; }

  // An Adventurer until told: the spells it knows, its power (full now), its auto attack
  void SetClass(UnitClass unitClass);
  ClassInfo const& GetClass() const { return *_class; }
  bool KnowsSpell(uint32 spellId) const;
  CombatMap* GetMap() const { return _map; }

  float GetX() const { return _x; }
  float GetY() const { return _y; }
  void Relocate(float x, float y) {
    _x = x;
    _y = y;
  }
  // Radians: 0 along +x, the field's y down
  float GetFacing() const { return _facing; }
  void SetFacing(float facing);
  // Between the edges of the two: 0 when they touch
  float GetDistance(Unit const& other) const;

  uint32 GetHealth() const { return _health; }
  uint32 GetMaxHealth() const { return _maxHealth; }
  // Mana or energy, as the class has
  uint32 GetMana() const { return _mana; }
  uint32 GetMaxMana() const { return _maxMana; }
  uint32 GetComboPoints() const { return _comboPoints; }
  bool IsAlive() const { return _health > 0; }

  // Percent of direct damage and heals that do half again as much
  uint32 GetCritChance() const { return _critChance; }
  void SetCritChance(uint32 percent) { _critChance = percent; }
  void SetManaRegen(uint32 perSecond) { _manaRegen = perSecond; }

  // What the unit looks at, shown to everyone; casting doesn't need it
  uint32 GetTargetId() const { return _targetId; }
  void SetTargetId(uint32 id) { _targetId = id; }

  // By whoever moves it, every update: a cast that takes time is interrupted when it starts moving
  void SetMoving(bool moving);
  bool IsMoving() const { return _moving; }
  bool CanMove() const { return IsAlive() && !IsStunned(); }

  // In a map. targetId is ignored by a spell at the caster itself
  CastResult CastSpell(uint32 spellId, uint32 targetId);
  void CancelCast();

  // Dead, NoTarget, TargetDead or BadTarget (itself) refuse it
  CastResult StartAttack(uint32 targetId);
  void StopAttack();
  uint32 GetAttackTargetId() const { return _attackTargetId; }
  bool IsCasting() const { return _castSpell != nullptr; }
  SpellInfo const* GetCurrentSpell() const { return _castSpell; }
  uint32 GetCastRemaining() const { return _castRemaining; }

  uint32 GetCooldown(uint32 spellId) const;  // ms left, 0 when ready
  uint32 GetGlobalCooldown() const { return _globalCooldown; }

  bool HasAura(uint32 auraId) const;
  bool IsStunned() const;
  // 1, more with a SpeedBoost
  float GetSpeedMultiplier() const;
  std::vector<Aura> const& GetAuras() const { return _auras; }

  // From an attacker, which may have left the map since (a damage over time). Amount 0 is nothing
  void TakeDamage(uint32 attackerId, uint32 spellId, uint32 amount, bool crit, bool periodic);
  void TakeHeal(uint32 healerId, uint32 spellId, uint32 amount, bool crit);
  // extraDuration: ms past the aura's own, as a finisher's combo points give
  void ApplyAura(uint32 casterId, AuraInfo const& info, uint32 extraDuration = 0);

  // Full health and power, nothing on cooldown, no combo points
  void Revive();

  // Cooldowns, the cast, auras, mana. CombatMap::Update for the units in it
  void Update(uint32 diff);

 private:
  friend class CombatMap;

  void Notify(CombatEvent const& event);
  void StartGlobalCooldown(SpellInfo const& spell);
  void FinishCast();
  void InterruptCast(InterruptReason reason);
  // The checks a cast that takes time does again when it ends
  CastResult CheckTarget(SpellInfo const& spell, Unit const* target) const;
  void Go(SpellInfo const& spell, Unit& target);
  void TeleportBehind(Unit& target);
  void UpdateAuras(uint32 diff);
  void UpdateAttack(uint32 diff);
  void Die(uint32 killerId);

  uint32 _id;
  CombatMap* _map = nullptr;

  ClassInfo const* _class;
  float _x = 0.0f;
  float _y = 0.0f;
  float _facing = 0.0f;
  bool _moving = false;

  uint32 _health;
  uint32 _maxHealth;
  uint32 _mana;
  uint32 _maxMana;
  uint32 _manaRegen = 5;
  uint32 _manaRegenTimer = 0;  // ms x points a second: a point each 1000 of it
  uint32 _comboPoints = 0;
  uint32 _critChance = 10;
  uint32 _targetId = 0;

  SpellInfo const* _castSpell = nullptr;
  uint32 _castTargetId = 0;
  uint32 _castRemaining = 0;

  struct Cooldown {
    uint32 SpellId;
    uint32 Remaining;
  };
  std::vector<Cooldown> _cooldowns;  // a handful, only the ones running
  uint32 _globalCooldown = 0;

  std::vector<Aura> _auras;

  uint32 _attackTargetId = 0;
  uint32 _swingTimer = 0;  // ms until the next swing may land
};

#endif  // ARENA_UNIT_H
