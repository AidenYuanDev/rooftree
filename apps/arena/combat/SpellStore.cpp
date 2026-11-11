/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "SpellInfo.h"
#include <algorithm>

namespace
{
enum AuraIds : uint32
{
    AURA_POISON         = 1,
    AURA_STUN           = 2,
    AURA_DEADLY_POISON  = 3,
    AURA_SPRINT         = 4,
    AURA_KIDNEY_SHOT    = 5,
};

// By id, ids from 1 without gaps: a lookup is an index. Players have 200 health; an adventurer has
// 100 mana and 5 a second back, a rogue 100 energy and 10 a second back
std::vector<SpellInfo> const Spells =
{
    {
        .Id = 1, .Name = "Strike", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 0, .Range = 5.0f,
        .Effects = { { .Type = SpellEffectType::Damage, .Min = 10, .Max = 14 } },
    },
    {
        .Id = 2, .Name = "Fireball", .Target = SpellTarget::Enemy,
        .CastTime = 1500ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 20, .Range = 300.0f,
        .Effects = { { .Type = SpellEffectType::Damage, .Min = 28, .Max = 36 } },
    },
    {
        .Id = 3, .Name = "Poison", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 15, .Range = 300.0f,
        .Effects = { { .Type = SpellEffectType::ApplyAura, .AuraId = AURA_POISON } },
    },
    {
        .Id = 4, .Name = "Heal", .Target = SpellTarget::Self,
        .CastTime = 2000ms, .Cooldown = 8000ms, .TriggersGcd = true, .ManaCost = 30, .Range = 0.0f,
        .Effects = { { .Type = SpellEffectType::Heal, .Min = 45, .Max = 55 } },
    },
    {
        .Id = 5, .Name = "Hammer", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 15000ms, .TriggersGcd = true, .ManaCost = 10, .Range = 5.0f,
        .Effects =
        {
            { .Type = SpellEffectType::Damage, .Min = 5, .Max = 8 },
            { .Type = SpellEffectType::ApplyAura, .AuraId = AURA_STUN },
        },
    },

    // The rogue: energy, combo points from the first two, spent by the next two
    {
        .Id = 6, .Name = "Sinister Strike", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 40, .Range = 5.0f,
        .Effects =
        {
            { .Type = SpellEffectType::Damage, .Min = 14, .Max = 18 },
            { .Type = SpellEffectType::AddComboPoint, .Min = 1 },
        },
    },
    {
        .Id = 7, .Name = "Poisoned Blade", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 30, .Range = 5.0f,
        .Effects =
        {
            { .Type = SpellEffectType::Damage, .Min = 5, .Max = 7 },
            { .Type = SpellEffectType::ApplyAura, .AuraId = AURA_DEADLY_POISON },
            { .Type = SpellEffectType::AddComboPoint, .Min = 1 },
        },
    },
    {
        .Id = 8, .Name = "Eviscerate", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 0ms, .TriggersGcd = true, .ManaCost = 35, .Range = 5.0f,
        .Effects = { { .Type = SpellEffectType::Damage, .Min = 6, .Max = 10, .PerComboPoint = 18 } },
        .Finisher = true,
    },
    {
        .Id = 9, .Name = "Kidney Shot", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 20000ms, .TriggersGcd = true, .ManaCost = 25, .Range = 5.0f,
        .Effects = { { .Type = SpellEffectType::ApplyAura, .AuraId = AURA_KIDNEY_SHOT, .PerComboPoint = 1000 } },
        .Finisher = true,
    },
    {
        .Id = 10, .Name = "Sprint", .Target = SpellTarget::Self,
        .CastTime = 0ms, .Cooldown = 30000ms, .TriggersGcd = false, .ManaCost = 0, .Range = 0.0f,
        .Effects = { { .Type = SpellEffectType::ApplyAura, .AuraId = AURA_SPRINT } },
    },
    {
        .Id = 11, .Name = "Shadowstep", .Target = SpellTarget::Enemy,
        .CastTime = 0ms, .Cooldown = 15000ms, .TriggersGcd = false, .ManaCost = 0, .Range = 300.0f,
        .Effects = { { .Type = SpellEffectType::TeleportBehind } },
    },
};

std::vector<AuraInfo> const Auras =
{
    { .Id = AURA_POISON, .Name = "Poison", .Type = AuraType::PeriodicDamage, .Duration = 6000ms, .Period = 1000ms, .Amount = 5 },
    { .Id = AURA_STUN,   .Name = "Stun",   .Type = AuraType::Stun,           .Duration = 2000ms },
    { .Id = AURA_DEADLY_POISON, .Name = "Deadly Poison", .Type = AuraType::PeriodicDamage, .Duration = 8000ms, .Period = 2000ms, .Amount = 6 },
    { .Id = AURA_SPRINT,        .Name = "Sprint",        .Type = AuraType::SpeedBoost,     .Duration = 4000ms, .Amount = 70 },
    // As long as the combo points spent say
    { .Id = AURA_KIDNEY_SHOT,   .Name = "Kidney Shot",   .Type = AuraType::Stun,           .Duration = 0ms },
};

std::vector<ClassInfo> const Classes =
{
    {
        .Class = UnitClass::Adventurer, .Name = "Adventurer", .Power = PowerType::Mana, .MaxPower = 100, .PowerRegen = 5,
        .AttackInterval = 2000, .AttackMinDamage = 8, .AttackMaxDamage = 12, .Spells = { 1, 2, 3, 4, 5 },
    },
    {
        // Two daggers: quicker, lighter swings
        .Class = UnitClass::Rogue, .Name = "Rogue", .Power = PowerType::Energy, .MaxPower = 100, .PowerRegen = 10,
        .AttackInterval = 1500, .AttackMinDamage = 6, .AttackMaxDamage = 9, .Spells = { 6, 7, 8, 9, 10, 11 },
    },
};

template <class Info>
Info const* Find(std::vector<Info> const& infos, uint32 id)
{
    return id && id <= infos.size() ? &infos[id - 1] : nullptr;
}
}

SpellInfo const* SpellStore::GetSpell(uint32 id)
{
    return Find(Spells, id);
}

AuraInfo const* SpellStore::GetAura(uint32 id)
{
    return Find(Auras, id);
}

std::span<SpellInfo const> SpellStore::GetSpells()
{
    return Spells;
}

ClassInfo const& SpellStore::GetClass(UnitClass unitClass)
{
    auto itr = std::ranges::find(Classes, unitClass, &ClassInfo::Class);
    return itr != Classes.end() ? *itr : Classes.front();
}
