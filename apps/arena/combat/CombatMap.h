/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The units that can fight each other, by id, and what happened between them since the last
 * TakeEvents. The map doesn't own its units, a unit leaves it when it is destroyed. One per world
 * for now; with several maps each would have its own, run on its own map thread.
 *
 * Each world update: units move, then Update(diff) runs casts, auras and regeneration, then the
 * events go to the clients. Casts started by messages in between add their events as they happen.
 */

#ifndef ARENA_COMBAT_MAP_H
#define ARENA_COMBAT_MAP_H

#include "Define.h"
#include <random>
#include <unordered_map>
#include <vector>

class Unit;

enum class CombatEventType : uint8
{
    CastStart,          // Duration: the cast time
    SpellGo,            // the cast completed, instant ones too; the effects' events follow
    CastInterrupted,    // Reason
    Damage,             // Amount done, Crit, Periodic (Spell is then the aura's)
    Heal,               // Amount done, Crit
    AuraApplied,        // Spell: the aura, Duration; also when one is refreshed
    AuraRemoved,        // Spell: the aura
    Death,              // Caster: the killer
    Swing               // an auto attack's blow at Target; its Damage (Spell 0) follows
};

struct CombatEvent
{
    CombatEventType Type;
    uint32 Caster;
    uint32 Target;
    uint32 Spell;
    uint32 Amount = 0;
    uint32 Duration = 0;    // ms
    bool Crit = false;
    bool Periodic = false;
    uint8 Reason = 0;       // InterruptReason, None for the other types
};

class CombatMap
{
public:
    CombatMap();
    // Rolls the same numbers each time, for tests
    explicit CombatMap(uint32 seed);
    ~CombatMap();

    CombatMap(CombatMap const&) = delete;
    CombatMap& operator=(CombatMap const&) = delete;

    // Ids are unique in a map. Adding a unit already in it does nothing
    void AddUnit(Unit& unit);
    void RemoveUnit(Unit& unit);
    Unit* FindUnit(uint32 id) const;
    std::size_t GetUnitCount() const { return _units.size(); }

    void Update(uint32 diff);

    void AddEvent(CombatEvent const& event) { _events.push_back(event); }
    std::vector<CombatEvent> TakeEvents();

    // Both ends included
    uint32 Roll(uint32 min, uint32 max);

private:
    std::unordered_map<uint32, Unit*> _units;
    std::vector<CombatEvent> _events;
    std::mt19937 _random;
};

#endif // ARENA_COMBAT_MAP_H
