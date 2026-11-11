/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "SpellInfo.h"

namespace
{
enum AuraIds : uint32
{
    AURA_POISON = 1,
    AURA_STUN   = 2,
};

// By id, ids from 1 without gaps: a lookup is an index. Players have 200 health, 100 mana and get
// 5 mana a second back
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
};

std::vector<AuraInfo> const Auras =
{
    { .Id = AURA_POISON, .Name = "Poison", .Type = AuraType::PeriodicDamage, .Duration = 6000ms, .Period = 1000ms, .Amount = 5 },
    { .Id = AURA_STUN,   .Name = "Stun",   .Type = AuraType::Stun,           .Duration = 2000ms },
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
