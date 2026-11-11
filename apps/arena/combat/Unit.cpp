/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Unit.h"
#include "CombatMap.h"
#include "Errors.h"
#include "SpellInfo.h"
#include <algorithm>
#include <cmath>
#include <utility>

Unit::Unit(uint32 id, uint32 maxHealth, uint32 maxMana)
    : _id(id), _class(&SpellStore::GetClass(UnitClass::Adventurer)), _health(maxHealth), _maxHealth(maxHealth), _mana(maxMana), _maxMana(maxMana)
{
}

void Unit::SetClass(UnitClass unitClass)
{
    _class = &SpellStore::GetClass(unitClass);
    _mana = _maxMana = _class->MaxPower;
    _manaRegen = _class->PowerRegen;
    _manaRegenTimer = 0;
    _comboPoints = 0;
}

bool Unit::KnowsSpell(uint32 spellId) const
{
    return std::ranges::find(_class->Spells, spellId) != _class->Spells.end();
}

void Unit::SetFacing(float facing)
{
    _facing = std::remainder(facing, 2.0f * float(M_PI));
}

Unit::~Unit()
{
    if (_map)
        _map->RemoveUnit(*this);
}

float Unit::GetDistance(Unit const& other) const
{
    float dx = other._x - _x;
    float dy = other._y - _y;
    return std::max(0.0f, std::sqrt(dx * dx + dy * dy) - 2 * Radius);
}

void Unit::SetMoving(bool moving)
{
    _moving = moving;
    if (moving && _castSpell)
        InterruptCast(InterruptReason::Moved);
}

CastResult Unit::CastSpell(uint32 spellId, uint32 targetId)
{
    ASSERT(_map, "unit %u casts outside a map", _id);

    SpellInfo const* spell = SpellStore::GetSpell(spellId);
    if (!spell || !KnowsSpell(spellId))
        return CastResult::UnknownSpell;
    if (!IsAlive())
        return CastResult::Dead;
    if (_castSpell)
        return CastResult::Casting;
    if (IsStunned())
        return CastResult::Stunned;
    if (spell->CastTime > 0ms && _moving)
        return CastResult::Moving;
    if (GetCooldown(spellId) || (spell->TriggersGcd && _globalCooldown))
        return CastResult::NotReady;
    if (_mana < spell->ManaCost)
        return CastResult::NoMana;
    if (spell->Finisher && !_comboPoints)
        return CastResult::NoComboPoints;

    Unit* target = this;
    if (spell->Target == SpellTarget::Enemy)
    {
        if (targetId == _id)
            return CastResult::BadTarget;
        target = targetId ? _map->FindUnit(targetId) : nullptr;
    }
    if (CastResult result = CheckTarget(*spell, target); result != CastResult::Ok)
        return result;

    StartGlobalCooldown(*spell);

    if (spell->CastTime == 0ms)
    {
        Go(*spell, *target);
        return CastResult::Ok;
    }

    _castSpell = spell;
    _castTargetId = target->GetId();
    _castRemaining = uint32(spell->CastTime.count());
    Notify({ .Type = CombatEventType::CastStart, .Caster = _id, .Target = _castTargetId, .Spell = spell->Id, .Duration = _castRemaining });
    return CastResult::Ok;
}

void Unit::CancelCast()
{
    if (_castSpell)
        InterruptCast(InterruptReason::Cancelled);
}

uint32 Unit::GetCooldown(uint32 spellId) const
{
    auto itr = std::ranges::find(_cooldowns, spellId, &Cooldown::SpellId);
    return itr != _cooldowns.end() ? itr->Remaining : 0;
}

bool Unit::HasAura(uint32 auraId) const
{
    return std::ranges::any_of(_auras, [auraId](Aura const& aura) { return aura.Info->Id == auraId; });
}

bool Unit::IsStunned() const
{
    return std::ranges::any_of(_auras, [](Aura const& aura) { return aura.Info->Type == AuraType::Stun; });
}

float Unit::GetSpeedMultiplier() const
{
    float multiplier = 1.0f;
    for (Aura const& aura : _auras)
        if (aura.Info->Type == AuraType::SpeedBoost)
            multiplier += float(aura.Info->Amount) / 100.0f;
    return multiplier;
}

void Unit::TakeDamage(uint32 attackerId, uint32 spellId, uint32 amount, bool crit, bool periodic)
{
    if (!IsAlive() || !amount)
        return;

    amount = std::min(amount, _health);
    _health -= amount;
    Notify({ .Type = CombatEventType::Damage, .Caster = attackerId, .Target = _id, .Spell = spellId, .Amount = amount, .Crit = crit, .Periodic = periodic });

    if (!_health)
        Die(attackerId);
}

void Unit::TakeHeal(uint32 healerId, uint32 spellId, uint32 amount, bool crit)
{
    if (!IsAlive())
        return;

    // What it did, not what it could have: a heal at full health is 0
    amount = std::min(amount, _maxHealth - _health);
    _health += amount;
    Notify({ .Type = CombatEventType::Heal, .Caster = healerId, .Target = _id, .Spell = spellId, .Amount = amount, .Crit = crit });
}

void Unit::ApplyAura(uint32 casterId, AuraInfo const& info, uint32 extraDuration /*= 0*/)
{
    if (!IsAlive())
        return;

    // The same one again from the same caster starts over; from another caster it is a second one
    uint32 duration = uint32(info.Duration.count()) + extraDuration;
    auto itr = std::ranges::find_if(_auras, [&](Aura const& aura) { return aura.Info == &info && aura.CasterId == casterId; });
    if (itr != _auras.end())
        *itr = { .Info = &info, .CasterId = casterId, .Remaining = duration };
    else
        _auras.push_back({ .Info = &info, .CasterId = casterId, .Remaining = duration });

    Notify({ .Type = CombatEventType::AuraApplied, .Caster = casterId, .Target = _id, .Spell = info.Id, .Duration = duration });

    if (info.Type == AuraType::Stun)
    {
        _moving = false;
        if (_castSpell)
            InterruptCast(InterruptReason::Stunned);
    }
}

CastResult Unit::StartAttack(uint32 targetId)
{
    if (!IsAlive())
        return CastResult::Dead;
    if (targetId == _id)
        return CastResult::BadTarget;
    Unit* target = _map ? _map->FindUnit(targetId) : nullptr;
    if (!target)
        return CastResult::NoTarget;
    if (!target->IsAlive())
        return CastResult::TargetDead;

    // Another target keeps the timer: switching doesn't give a free swing
    _attackTargetId = targetId;
    return CastResult::Ok;
}

void Unit::StopAttack()
{
    _attackTargetId = 0;
}

void Unit::Revive()
{
    _health = _maxHealth;
    _mana = _maxMana;
    _manaRegenTimer = 0;
    _comboPoints = 0;
    _cooldowns.clear();
    _globalCooldown = 0;
    _swingTimer = 0;
}

void Unit::Update(uint32 diff)
{
    for (Cooldown& cooldown : _cooldowns)
        cooldown.Remaining -= std::min(cooldown.Remaining, diff);
    std::erase_if(_cooldowns, [](Cooldown const& cooldown) { return !cooldown.Remaining; });
    _globalCooldown -= std::min(_globalCooldown, diff);

    if (!IsAlive())
        return;

    // Point by point as they come (energy fills smoothly), the remainder carries over
    _manaRegenTimer += diff * _manaRegen;
    _mana = std::min(_maxMana, _mana + _manaRegenTimer / 1000);
    _manaRegenTimer %= 1000;

    if (_castSpell)
    {
        _castRemaining -= std::min(_castRemaining, diff);
        if (!_castRemaining)
            FinishCast();
    }

    UpdateAttack(diff);
    UpdateAuras(diff);
}

void Unit::UpdateAttack(uint32 diff)
{
    _swingTimer -= std::min(_swingTimer, diff);
    if (!_attackTargetId)
        return;

    Unit* target = _map ? _map->FindUnit(_attackTargetId) : nullptr;
    if (!target || !target->IsAlive())
        return StopAttack();
    if (_swingTimer || IsCasting() || IsStunned() || GetDistance(*target) > MeleeRange)
        return;

    _swingTimer = _class->AttackInterval;
    uint32 amount = _map->Roll(_class->AttackMinDamage, _class->AttackMaxDamage);
    bool crit = _map->Roll(1, 100) <= _critChance;
    if (crit)
        amount += amount / 2;
    Notify({ .Type = CombatEventType::Swing, .Caster = _id, .Target = target->GetId() });
    target->TakeDamage(_id, 0, amount, crit, false);
}

void Unit::Notify(CombatEvent const& event)
{
    // Outside a map nobody hears it
    if (_map)
        _map->AddEvent(event);
}

void Unit::StartGlobalCooldown(SpellInfo const& spell)
{
    if (spell.TriggersGcd)
        _globalCooldown = uint32(SpellStore::GlobalCooldown.count());
}

void Unit::FinishCast()
{
    SpellInfo const& spell = *_castSpell;
    Unit* target = spell.Target == SpellTarget::Self ? this : _map->FindUnit(_castTargetId);

    switch (CheckTarget(spell, target))
    {
        case CastResult::Ok:
            break;
        case CastResult::OutOfRange:
            InterruptCast(InterruptReason::OutOfRange);
            return;
        default:
            InterruptCast(InterruptReason::TargetLost);
            return;
    }

    _castSpell = nullptr;
    _castRemaining = 0;
    Go(spell, *target);
}

void Unit::InterruptCast(InterruptReason reason)
{
    ASSERT(_castSpell);
    Notify({ .Type = CombatEventType::CastInterrupted, .Caster = _id, .Target = _castTargetId, .Spell = _castSpell->Id, .Reason = uint8(reason) });
    _castSpell = nullptr;
    _castRemaining = 0;
}

CastResult Unit::CheckTarget(SpellInfo const& spell, Unit const* target) const
{
    if (!target)
        return CastResult::NoTarget;
    if (!target->IsAlive())
        return CastResult::TargetDead;
    if (target != this && GetDistance(*target) > spell.Range)
        return CastResult::OutOfRange;
    return CastResult::Ok;
}

void Unit::Go(SpellInfo const& spell, Unit& target)
{
    _mana -= spell.ManaCost;
    if (spell.Cooldown > 0ms)
        _cooldowns.push_back({ .SpellId = spell.Id, .Remaining = uint32(spell.Cooldown.count()) });

    Notify({ .Type = CombatEventType::SpellGo, .Caster = _id, .Target = target.GetId(), .Spell = spell.Id });

    // A finisher spends them all, its effects grow with them
    uint32 spent = spell.Finisher ? std::exchange(_comboPoints, 0) : 0;

    for (SpellEffect const& effect : spell.Effects)
    {
        // The first effect may have killed it
        if (!target.IsAlive())
            break;

        switch (effect.Type)
        {
            case SpellEffectType::Damage:
            case SpellEffectType::Heal:
            {
                uint32 amount = _map->Roll(uint32(effect.Min), uint32(effect.Max)) + uint32(effect.PerComboPoint) * spent;
                bool crit = _map->Roll(1, 100) <= _critChance;
                if (crit)
                    amount = amount * 3 / 2;

                if (effect.Type == SpellEffectType::Damage)
                    target.TakeDamage(_id, spell.Id, amount, crit, false);
                else
                    target.TakeHeal(_id, spell.Id, amount, crit);
                break;
            }
            case SpellEffectType::ApplyAura:
                if (AuraInfo const* aura = SpellStore::GetAura(effect.AuraId))
                    target.ApplyAura(_id, *aura, uint32(effect.PerComboPoint) * spent);
                break;
            case SpellEffectType::AddComboPoint:
                _comboPoints = std::min(_comboPoints + uint32(effect.Min), SpellStore::MaxComboPoints);
                break;
            case SpellEffectType::TeleportBehind:
                TeleportBehind(target);
                break;
        }
    }
}

void Unit::TeleportBehind(Unit& target)
{
    // Just touching its back, kept inside the map's field; then facing it
    float behind = 2 * Radius + 1.0f;
    float x = target.GetX() - std::cos(target.GetFacing()) * behind;
    float y = target.GetY() - std::sin(target.GetFacing()) * behind;
    _map->ClampToField(x, y, Radius);
    Relocate(x, y);
    SetFacing(std::atan2(target.GetY() - y, target.GetX() - x));
}

void Unit::UpdateAuras(uint32 diff)
{
    for (std::size_t i = 0; i < _auras.size(); ++i)
    {
        Aura& aura = _auras[i];
        aura.Remaining -= std::min(aura.Remaining, diff);

        if (aura.Info->Type != AuraType::PeriodicDamage)
            continue;

        // Several ticks in one update when it was long; the last one when the aura ends
        uint32 period = uint32(aura.Info->Period.count());
        uint32 ticks = uint32(aura.Info->Duration.count()) / period;
        aura.TickTimer += diff;
        while (aura.TickTimer >= period && aura.Ticks < ticks)
        {
            aura.TickTimer -= period;
            ++aura.Ticks;
            TakeDamage(aura.CasterId, aura.Info->Id, uint32(aura.Info->Amount), false, true);
            // Died of it: Die removed every aura
            if (!IsAlive())
                return;
        }
    }

    std::erase_if(_auras, [this](Aura const& aura)
    {
        if (aura.Remaining)
            return false;
        Notify({ .Type = CombatEventType::AuraRemoved, .Caster = aura.CasterId, .Target = _id, .Spell = aura.Info->Id });
        return true;
    });
}

void Unit::Die(uint32 killerId)
{
    _moving = false;
    _comboPoints = 0;
    StopAttack();
    if (_castSpell)
        InterruptCast(InterruptReason::Died);

    for (Aura const& aura : _auras)
        Notify({ .Type = CombatEventType::AuraRemoved, .Caster = aura.CasterId, .Target = _id, .Spell = aura.Info->Id });
    _auras.clear();

    Notify({ .Type = CombatEventType::Death, .Caster = killerId, .Target = _id });
}
