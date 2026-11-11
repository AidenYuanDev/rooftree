/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include <catch2/catch_all.hpp>

#include "CombatMap.h"
#include "SpellInfo.h"
#include "Unit.h"
#include <algorithm>

namespace
{
enum : uint32
{
    SPELL_STRIKE   = 1,
    SPELL_FIREBALL = 2,
    SPELL_POISON   = 3,
    SPELL_HEAL     = 4,
    SPELL_HAMMER   = 5,

    AURA_POISON = 1,
    AURA_STUN   = 2,
};

ClassInfo const& Adventurer()
{
    return SpellStore::GetClass(UnitClass::Adventurer);
}

// Two units 0 apart at the edges (strike range), no crits unless a test wants them
struct Duel
{
    CombatMap Map{ 42 };
    Unit A{ 1, 200, 100 };
    Unit B{ 2, 200, 100 };

    Duel()
    {
        A.Relocate(100.0f, 100.0f);
        B.Relocate(100.0f + 2 * Unit::Radius, 100.0f);
        A.SetCritChance(0);
        B.SetCritChance(0);
        Map.AddUnit(A);
        Map.AddUnit(B);
    }

    // Events of one type since the last call, the others dropped
    std::vector<CombatEvent> Take(CombatEventType type)
    {
        std::vector<CombatEvent> events = Map.TakeEvents();
        std::erase_if(events, [type](CombatEvent const& event) { return event.Type != type; });
        return events;
    }

    void Wait(uint32 ms)
    {
        Map.Update(ms);
    }
};
}

TEST_CASE("Spell and aura ids are their places in the table", "[combat]")
{
    std::span<SpellInfo const> spells = SpellStore::GetSpells();
    REQUIRE(!spells.empty());
    for (SpellInfo const& spell : spells)
        CHECK(SpellStore::GetSpell(spell.Id) == &spell);

    CHECK(SpellStore::GetSpell(0) == nullptr);
    CHECK(SpellStore::GetSpell(uint32(spells.size()) + 1) == nullptr);
    REQUIRE(SpellStore::GetAura(AURA_POISON));
    CHECK(SpellStore::GetAura(AURA_POISON)->Type == AuraType::PeriodicDamage);
    REQUIRE(SpellStore::GetAura(AURA_STUN));
    CHECK(SpellStore::GetAura(AURA_STUN)->Type == AuraType::Stun);
    CHECK(SpellStore::GetAura(6) == nullptr);
}

TEST_CASE("An instant spell hits at once and starts the global cooldown", "[combat]")
{
    Duel duel;
    REQUIRE(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);

    std::vector<CombatEvent> events = duel.Map.TakeEvents();
    REQUIRE(events.size() == 2);
    CHECK(events[0].Type == CombatEventType::SpellGo);
    CHECK(events[1].Type == CombatEventType::Damage);
    CHECK(events[1].Caster == 1);
    CHECK(events[1].Target == 2);
    CHECK(events[1].Amount >= 10);
    CHECK(events[1].Amount <= 14);
    CHECK(duel.B.GetHealth() == 200 - events[1].Amount);

    CHECK(duel.A.GetGlobalCooldown() == 1000);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::NotReady);
    duel.Wait(999);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::NotReady);
    duel.Wait(1);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);
}

TEST_CASE("A cast is refused for what the caster can't do", "[combat]")
{
    Duel duel;
    CHECK(duel.A.CastSpell(99, duel.B.GetId()) == CastResult::UnknownSpell);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, 0) == CastResult::NoTarget);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, 77) == CastResult::NoTarget);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.A.GetId()) == CastResult::BadTarget);

    // Strike reaches 5 past touching, Fireball 300
    duel.B.Relocate(100.0f + 2 * Unit::Radius + 6.0f, 100.0f);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::OutOfRange);
    duel.B.Relocate(100.0f + 2 * Unit::Radius + 5.0f, 100.0f);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);
    duel.Wait(1000);

    duel.B.Relocate(100.0f + 2 * Unit::Radius + 301.0f, 100.0f);
    CHECK(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::OutOfRange);

    // Nothing refused started anything
    CHECK(duel.A.GetGlobalCooldown() == 0);
    CHECK(!duel.A.IsCasting());
    CHECK(duel.A.GetMana() == 100);
}

TEST_CASE("A spell that takes time completes after its cast time and spends mana then", "[combat]")
{
    Duel duel;
    duel.B.Relocate(300.0f, 100.0f);
    REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);

    std::vector<CombatEvent> starts = duel.Take(CombatEventType::CastStart);
    REQUIRE(starts.size() == 1);
    CHECK(starts[0].Spell == SPELL_FIREBALL);
    CHECK(starts[0].Duration == 1500);
    CHECK(duel.A.IsCasting());
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Casting);

    duel.A.SetManaRegen(0);
    duel.Wait(1499);
    CHECK(duel.A.IsCasting());
    CHECK(duel.A.GetMana() == 100);
    CHECK(duel.B.GetHealth() == 200);

    duel.Wait(1);
    CHECK(!duel.A.IsCasting());
    CHECK(duel.A.GetMana() == 80);
    std::vector<CombatEvent> hits = duel.Take(CombatEventType::Damage);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].Spell == SPELL_FIREBALL);
    CHECK(hits[0].Amount >= 28);
    CHECK(hits[0].Amount <= 36);
}

TEST_CASE("Moving interrupts a cast and keeps one from starting", "[combat]")
{
    Duel duel;
    REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();
    duel.Wait(500);

    duel.A.SetMoving(true);
    CHECK(!duel.A.IsCasting());
    std::vector<CombatEvent> interrupts = duel.Take(CombatEventType::CastInterrupted);
    REQUIRE(interrupts.size() == 1);
    CHECK(interrupts[0].Reason == uint8(InterruptReason::Moved));
    CHECK(duel.A.GetMana() == 100);

    // Instants are cast on the run
    duel.Wait(1000);
    CHECK(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Moving);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);

    duel.Wait(2000);
    CHECK(duel.B.GetHealth() < 200);
}

TEST_CASE("A cast whose target left, died or walked away fails when it ends", "[combat]")
{
    Duel duel;

    SECTION("out of range")
    {
        REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);
        duel.B.Relocate(1000.0f, 100.0f);
        duel.Map.TakeEvents();
        duel.Wait(1500);
        std::vector<CombatEvent> interrupts = duel.Take(CombatEventType::CastInterrupted);
        REQUIRE(interrupts.size() == 1);
        CHECK(interrupts[0].Reason == uint8(InterruptReason::OutOfRange));
        CHECK(duel.A.GetMana() == 100);
    }

    SECTION("left the map")
    {
        REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);
        duel.Map.RemoveUnit(duel.B);
        duel.Map.TakeEvents();
        duel.Wait(1500);
        std::vector<CombatEvent> interrupts = duel.Take(CombatEventType::CastInterrupted);
        REQUIRE(interrupts.size() == 1);
        CHECK(interrupts[0].Reason == uint8(InterruptReason::TargetLost));
    }

    SECTION("died")
    {
        REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);
        duel.B.TakeDamage(3, 0, 1000, false, false);
        duel.Map.TakeEvents();
        duel.Wait(1500);
        std::vector<CombatEvent> interrupts = duel.Take(CombatEventType::CastInterrupted);
        REQUIRE(interrupts.size() == 1);
        CHECK(interrupts[0].Reason == uint8(InterruptReason::TargetLost));
        CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::TargetDead);
    }
}

TEST_CASE("A damage over time ticks every period, the last tick when it ends", "[combat]")
{
    Duel duel;
    REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
    std::vector<CombatEvent> applied = duel.Take(CombatEventType::AuraApplied);
    REQUIRE(applied.size() == 1);
    CHECK(applied[0].Spell == AURA_POISON);
    CHECK(applied[0].Duration == 6000);
    CHECK(duel.B.HasAura(AURA_POISON));

    SECTION("an update at a time")
    {
        uint32 total = 0;
        for (int i = 0; i < 6; ++i)
        {
            duel.Wait(999);
            CHECK(duel.Take(CombatEventType::Damage).empty());
            duel.Wait(1);
            std::vector<CombatEvent> ticks = duel.Take(CombatEventType::Damage);
            REQUIRE(ticks.size() == 1);
            CHECK(ticks[0].Periodic);
            CHECK(ticks[0].Caster == 1);
            CHECK(ticks[0].Spell == AURA_POISON);
            total += ticks[0].Amount;
        }
        CHECK(total == 30);
        CHECK(!duel.B.HasAura(AURA_POISON));
    }

    SECTION("one long update")
    {
        duel.Wait(10000);
        std::vector<CombatEvent> events = duel.Map.TakeEvents();
        CHECK(std::ranges::count(events, CombatEventType::Damage, &CombatEvent::Type) == 6);
        CHECK(std::ranges::count(events, CombatEventType::AuraRemoved, &CombatEvent::Type) == 1);
        CHECK(duel.B.GetHealth() == 170);
    }

    SECTION("cast again it starts over, another caster's is a second one")
    {
        duel.Wait(3000);
        REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
        CHECK(duel.B.GetAuras().size() == 1);
        CHECK(duel.B.GetAuras()[0].Remaining == 6000);

        Unit c(3, 200, 100);
        c.Relocate(200.0f, 200.0f);
        duel.Map.AddUnit(c);
        REQUIRE(c.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
        CHECK(duel.B.GetAuras().size() == 2);

        duel.Map.TakeEvents();
        duel.Wait(1000);
        CHECK(duel.Take(CombatEventType::Damage).size() == 2);
    }
}

TEST_CASE("A stun interrupts the target's cast and keeps it from moving and casting", "[combat]")
{
    Duel duel;
    REQUIRE(duel.B.CastSpell(SPELL_FIREBALL, duel.A.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();

    REQUIRE(duel.A.CastSpell(SPELL_HAMMER, duel.B.GetId()) == CastResult::Ok);
    std::vector<CombatEvent> events = duel.Map.TakeEvents();
    REQUIRE(events.size() == 4);
    CHECK(events[0].Type == CombatEventType::SpellGo);
    CHECK(events[1].Type == CombatEventType::Damage);
    CHECK(events[2].Type == CombatEventType::AuraApplied);
    CHECK(events[3].Type == CombatEventType::CastInterrupted);
    CHECK(events[3].Caster == 2);
    CHECK(events[3].Reason == uint8(InterruptReason::Stunned));

    CHECK(duel.B.IsStunned());
    CHECK(!duel.B.CanMove());
    CHECK(duel.B.CastSpell(SPELL_STRIKE, duel.A.GetId()) == CastResult::Stunned);

    duel.Wait(2000);
    CHECK(!duel.B.IsStunned());
    CHECK(duel.B.CanMove());
    CHECK(duel.B.CastSpell(SPELL_STRIKE, duel.A.GetId()) == CastResult::Ok);

    // Hammer's own cooldown outlasts the global one
    CHECK(duel.A.GetCooldown(SPELL_HAMMER) == 13000);
    CHECK(duel.A.CastSpell(SPELL_HAMMER, duel.B.GetId()) == CastResult::NotReady);
    duel.Wait(13000);
    CHECK(duel.A.CastSpell(SPELL_HAMMER, duel.B.GetId()) == CastResult::Ok);
}

TEST_CASE("A heal is cast at the caster and heals no more than it lost", "[combat]")
{
    Duel duel;
    duel.A.TakeDamage(2, 0, 20, false, false);
    REQUIRE(duel.A.CastSpell(SPELL_HEAL, duel.B.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();
    duel.Wait(2000);

    std::vector<CombatEvent> heals = duel.Take(CombatEventType::Heal);
    REQUIRE(heals.size() == 1);
    CHECK(heals[0].Target == 1);
    CHECK(heals[0].Amount == 20);
    CHECK(duel.A.GetHealth() == 200);
    CHECK(duel.B.GetHealth() == 200);
    CHECK(duel.A.GetCooldown(SPELL_HEAL) == 8000);
}

TEST_CASE("Mana runs out and comes back", "[combat]")
{
    Duel duel;
    duel.B.Relocate(300.0f, 100.0f);
    // 15 each: 6 casts leave 10
    for (int i = 0; i < 6; ++i)
    {
        REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
        duel.A.Update(1000);    // the global cooldown, and 5 mana back
    }
    CHECK(duel.A.GetMana() == 100 - 6 * 15 + 6 * 5);

    duel.A.SetManaRegen(0);
    REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
    duel.A.Update(1000);
    REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
    duel.A.Update(1000);
    CHECK(duel.A.GetMana() == 10);
    CHECK(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::NoMana);

    // Point by point: 5 a second is one every 200 ms
    duel.A.SetManaRegen(5);
    duel.A.Update(199);
    CHECK(duel.A.GetMana() == 10);
    duel.A.Update(1);
    CHECK(duel.A.GetMana() == 11);
    duel.A.Update(800);
    CHECK(duel.A.GetMana() == 15);
    CHECK(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
}

TEST_CASE("A crit does half again as much", "[combat]")
{
    Duel duel;
    duel.A.SetCritChance(100);
    REQUIRE(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);
    std::vector<CombatEvent> hits = duel.Take(CombatEventType::Damage);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].Crit);
    CHECK(hits[0].Amount >= 15);
    CHECK(hits[0].Amount <= 21);
}

TEST_CASE("A unit dies at 0 health, its killer named, and fights no more until revived", "[combat]")
{
    Duel duel;
    REQUIRE(duel.B.CastSpell(SPELL_FIREBALL, duel.A.GetId()) == CastResult::Ok);
    REQUIRE(duel.A.CastSpell(SPELL_POISON, duel.B.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();

    // The poison's tick on 1 health left is the killing blow, its caster the killer
    duel.B.TakeDamage(1, 0, 199, false, false);
    duel.Map.TakeEvents();
    duel.Wait(1000);

    std::vector<CombatEvent> events = duel.Map.TakeEvents();
    auto death = std::ranges::find(events, CombatEventType::Death, &CombatEvent::Type);
    REQUIRE(death != events.end());
    CHECK(death->Target == 2);
    CHECK(death->Caster == 1);
    CHECK(std::ranges::count(events, CombatEventType::CastInterrupted, &CombatEvent::Type) == 1);
    CHECK(std::ranges::count(events, CombatEventType::AuraRemoved, &CombatEvent::Type) == 1);

    CHECK(!duel.B.IsAlive());
    CHECK(!duel.B.IsCasting());
    CHECK(duel.B.GetAuras().empty());
    CHECK(!duel.B.CanMove());
    CHECK(duel.B.CastSpell(SPELL_STRIKE, duel.A.GetId()) == CastResult::Dead);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::TargetDead);

    // Dead is dead: no more damage, heals or auras
    duel.B.TakeHeal(2, SPELL_HEAL, 50, false);
    duel.B.ApplyAura(1, *SpellStore::GetAura(AURA_POISON));
    CHECK(duel.B.GetHealth() == 0);
    CHECK(duel.B.GetAuras().empty());

    duel.B.Revive();
    CHECK(duel.B.IsAlive());
    CHECK(duel.B.GetHealth() == 200);
    CHECK(duel.B.GetMana() == 100);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);
}

TEST_CASE("Two units fight until one is dead", "[combat]")
{
    // Both in strike range, A strikes, B casts fireballs: who wins doesn't matter, that it ends does
    Duel duel;
    duel.A.SetCritChance(10);
    duel.B.SetCritChance(10);

    uint32 elapsed = 0;
    while (duel.A.IsAlive() && duel.B.IsAlive() && elapsed < 120000)
    {
        duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId());
        duel.B.CastSpell(SPELL_FIREBALL, duel.A.GetId());
        duel.Wait(50);
        elapsed += 50;
    }

    CHECK(duel.A.IsAlive() != duel.B.IsAlive());
    std::vector<CombatEvent> deaths = duel.Take(CombatEventType::Death);
    REQUIRE(deaths.size() == 1);
    Unit const& winner = duel.A.IsAlive() ? duel.A : duel.B;
    CHECK(deaths[0].Caster == winner.GetId());
}

TEST_CASE("A unit leaves its map when it is destroyed", "[combat]")
{
    CombatMap map(1);
    {
        Unit unit(5, 10, 10);
        map.AddUnit(unit);
        map.AddUnit(unit);
        CHECK(map.GetUnitCount() == 1);
        CHECK(map.FindUnit(5) == &unit);
    }
    CHECK(map.GetUnitCount() == 0);
    CHECK(map.FindUnit(5) == nullptr);

    // And the other way round
    Unit unit(6, 10, 10);
    {
        CombatMap other(1);
        other.AddUnit(unit);
        CHECK(unit.GetMap() == &other);
    }
    CHECK(unit.GetMap() == nullptr);
}

TEST_CASE("An auto attack swings at once, then every interval, and doesn't use the global cooldown", "[combat]")
{
    Duel duel;
    REQUIRE(duel.A.StartAttack(duel.B.GetId()) == CastResult::Ok);
    CHECK(duel.A.GetAttackTargetId() == duel.B.GetId());

    duel.Wait(1);
    std::vector<CombatEvent> events = duel.Map.TakeEvents();
    REQUIRE(events.size() == 2);
    CHECK(events[0].Type == CombatEventType::Swing);
    CHECK(events[0].Caster == 1);
    CHECK(events[0].Target == 2);
    CHECK(events[1].Type == CombatEventType::Damage);
    CHECK(events[1].Spell == 0);
    CHECK(events[1].Amount >= Adventurer().AttackMinDamage);
    CHECK(events[1].Amount <= Adventurer().AttackMaxDamage);
    CHECK(duel.A.GetGlobalCooldown() == 0);
    CHECK(duel.A.CastSpell(SPELL_STRIKE, duel.B.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();

    duel.Wait(Adventurer().AttackInterval - 1);
    CHECK(duel.Take(CombatEventType::Swing).empty());
    duel.Wait(1);
    CHECK(duel.Take(CombatEventType::Swing).size() == 1);
}

TEST_CASE("An auto attack waits out of range and swings as soon as the target is back", "[combat]")
{
    Duel duel;
    duel.B.Relocate(100.0f + 2 * Unit::Radius + Unit::MeleeRange + 1.0f, 100.0f);
    REQUIRE(duel.A.StartAttack(duel.B.GetId()) == CastResult::Ok);
    duel.Wait(5000);
    CHECK(duel.Take(CombatEventType::Swing).empty());

    duel.B.Relocate(100.0f + 2 * Unit::Radius + Unit::MeleeRange, 100.0f);
    duel.Wait(1);
    CHECK(duel.Take(CombatEventType::Swing).size() == 1);
}

TEST_CASE("No swing while casting or stunned", "[combat]")
{
    Duel duel;
    REQUIRE(duel.A.CastSpell(SPELL_FIREBALL, duel.B.GetId()) == CastResult::Ok);
    REQUIRE(duel.A.StartAttack(duel.B.GetId()) == CastResult::Ok);
    duel.Wait(1000);
    CHECK(duel.Take(CombatEventType::Swing).empty());
    // The fireball goes at 1500, the swing in the next update
    duel.Wait(500);
    duel.Wait(1);
    CHECK(duel.Take(CombatEventType::Swing).size() == 1);

    // Stunned (2 s) halfway to the next swing: it is due 1 s into the stun and waits for its end
    duel.Wait(1000);
    REQUIRE(duel.B.CastSpell(SPELL_HAMMER, duel.A.GetId()) == CastResult::Ok);
    duel.Map.TakeEvents();
    duel.Wait(1500);
    CHECK(duel.Take(CombatEventType::Swing).empty());
    // The stun runs out at the end of this update, the swing goes in the next
    duel.Wait(600);
    duel.Wait(1);
    CHECK(duel.Take(CombatEventType::Swing).size() == 1);
}

TEST_CASE("An auto attack is refused or stops for whom it can't hit", "[combat]")
{
    Duel duel;
    CHECK(duel.A.StartAttack(duel.A.GetId()) == CastResult::BadTarget);
    CHECK(duel.A.StartAttack(77) == CastResult::NoTarget);
    CHECK(duel.A.GetAttackTargetId() == 0);

    // The target dies: the attack ends with it
    REQUIRE(duel.A.StartAttack(duel.B.GetId()) == CastResult::Ok);
    duel.B.TakeDamage(3, 0, duel.B.GetHealth(), false, false);
    duel.Wait(1);
    CHECK(duel.A.GetAttackTargetId() == 0);
    CHECK(duel.Take(CombatEventType::Swing).empty());
    CHECK(duel.A.StartAttack(duel.B.GetId()) == CastResult::TargetDead);

    // The attacker dies
    duel.B.Revive();
    REQUIRE(duel.A.StartAttack(duel.B.GetId()) == CastResult::Ok);
    duel.A.TakeDamage(3, 0, duel.A.GetHealth(), false, false);
    CHECK(duel.A.GetAttackTargetId() == 0);
    CHECK(duel.A.StartAttack(duel.B.GetId()) == CastResult::Dead);
}
