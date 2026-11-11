/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What spells and auras do, read only. The table is written here for now (SpellStore.cpp); it is
 * what a client is sent at login, so it would move to the database or a data file unchanged.
 *
 * A spell is cast at a target (an enemy, or the caster itself), takes CastTime (0: instant), costs
 * mana when it completes and then has its Cooldown. One that TriggersGcd also starts the caster's
 * global cooldown when it starts, which keeps instants from being cast all in the same tick.
 * When it completes its effects run in order on the target.
 */

#ifndef ARENA_SPELL_INFO_H
#define ARENA_SPELL_INFO_H

#include "Define.h"
#include "Duration.h"
#include <span>
#include <string_view>
#include <vector>

enum class SpellTarget : uint8
{
    Enemy,      // any other living unit: everyone fights everyone for now
    Self
};

enum class SpellEffectType : uint8
{
    Damage,     // Min..Max, may crit
    Heal,       // Min..Max, may crit
    ApplyAura   // AuraId, from the caster
};

struct SpellEffect
{
    SpellEffectType Type;
    int32 Min = 0;
    int32 Max = 0;
    uint32 AuraId = 0;
};

struct SpellInfo
{
    uint32 Id;
    std::string_view Name;
    SpellTarget Target;
    Milliseconds CastTime;
    Milliseconds Cooldown;
    bool TriggersGcd;
    uint32 ManaCost;
    float Range;                        // between the two units' edges, see Unit::GetDistance
    std::vector<SpellEffect> Effects;
};

enum class AuraType : uint8
{
    PeriodicDamage,     // Amount every Period
    Stun                // no moving, no casting, a cast in progress is interrupted
};

struct AuraInfo
{
    uint32 Id;
    std::string_view Name;
    AuraType Type;
    Milliseconds Duration;
    Milliseconds Period = 0ms;          // PeriodicDamage: Duration / Period ticks, the last when it ends
    int32 Amount = 0;
};

namespace SpellStore
{
    // Every unit's global cooldown
    constexpr Milliseconds GlobalCooldown = 1000ms;

    // nullptr for an id that isn't one
    SpellInfo const* GetSpell(uint32 id);
    AuraInfo const* GetAura(uint32 id);

    // By id, what a client is sent
    std::span<SpellInfo const> GetSpells();
}

#endif // ARENA_SPELL_INFO_H
