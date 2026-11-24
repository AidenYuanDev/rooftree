/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "CombatMap.h"
#include "SpellInfo.h"
#include "Unit.h"

#include <catch2/catch_all.hpp>

#include <cmath>  // M_PI

namespace {
enum : uint32 {
  SPELL_STRIKE          = 1,
  SPELL_SINISTER_STRIKE = 6,
  SPELL_POISONED_BLADE  = 7,
  SPELL_EVISCERATE      = 8,
  SPELL_KIDNEY_SHOT     = 9,
  SPELL_SPRINT          = 10,
  SPELL_SHADOWSTEP      = 11,

  AURA_DEADLY_POISON = 3,
  AURA_SPRINT        = 4,
  AURA_KIDNEY_SHOT   = 5,
};

// A rogue touching a victim (an adventurer) on a 1000 x 700 field, no crits
struct Ambush {
  CombatMap Map{42};
  Unit Rogue{1, 200, 100};
  Unit Victim{2, 200, 100};

  Ambush() {
    Map.SetField(1000.0f, 700.0f);
    Rogue.SetClass(UnitClass::Rogue);
    Rogue.Relocate(100.0f, 100.0f);
    Victim.Relocate(100.0f + 2 * Unit::Radius, 100.0f);
    Rogue.SetCritChance(0);
    Victim.SetCritChance(0);
    Map.AddUnit(Rogue);
    Map.AddUnit(Victim);
  }

  std::vector<CombatEvent> Take(CombatEventType type) {
    std::vector<CombatEvent> events = Map.TakeEvents();
    std::erase_if(events, [type](CombatEvent const& event) { return event.Type != type; });
    return events;
  }

  // Past the global cooldown, energy coming back meanwhile
  void Wait(uint32 ms = 1000) { Map.Update(ms); }

  // Combo points by Sinister Strikes, waiting out the global cooldown between them
  void Build(uint32 points) {
    for (uint32 i = 0; i < points; ++i) {
      Rogue.SetManaRegen(0);
      REQUIRE(Rogue.CastSpell(SPELL_SINISTER_STRIKE, Victim.GetId()) == CastResult::Ok);
      Wait();
      Rogue.SetManaRegen(100);
      Wait();  // energy full again
    }
    Rogue.SetManaRegen(10);
    Map.TakeEvents();
  }
};
}  // namespace

TEST_CASE("A class knows its own spells only", "[rogue]") {
  Ambush ambush;
  CHECK(ambush.Rogue.CastSpell(SPELL_STRIKE, ambush.Victim.GetId()) == CastResult::UnknownSpell);
  CHECK(
      ambush.Victim.CastSpell(SPELL_SINISTER_STRIKE, ambush.Rogue.GetId()) ==
      CastResult::UnknownSpell);
  CHECK(ambush.Rogue.GetClass().Power == PowerType::Energy);
  CHECK(ambush.Rogue.GetClass().Spells.size() == 6);
}

TEST_CASE("Energy: 100, spent by the strikes, 10 a second back", "[rogue]") {
  Ambush ambush;
  CHECK(ambush.Rogue.GetMana() == 100);
  CHECK(ambush.Rogue.GetMaxMana() == 100);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.GetMana() == 60);
  ambush.Wait(100);
  CHECK(ambush.Rogue.GetMana() == 61);
  ambush.Wait(900);
  CHECK(ambush.Rogue.GetMana() == 70);

  REQUIRE(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
  ambush.Wait();
  CHECK(ambush.Rogue.GetMana() == 40);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
  ambush.Wait();
  CHECK(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::NoMana);
}

TEST_CASE("The builders add a combo point each, five at most", "[rogue]") {
  Ambush ambush;
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.GetComboPoints() == 1);
  ambush.Wait();
  REQUIRE(ambush.Rogue.CastSpell(SPELL_POISONED_BLADE, ambush.Victim.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.GetComboPoints() == 2);
  CHECK(ambush.Victim.HasAura(AURA_DEADLY_POISON));

  ambush.Wait();
  ambush.Build(5);
  CHECK(ambush.Rogue.GetComboPoints() == 5);
}

TEST_CASE("A finisher needs a combo point, spends them all and grows with them", "[rogue]") {
  Ambush ambush;
  CHECK(
      ambush.Rogue.CastSpell(SPELL_EVISCERATE, ambush.Victim.GetId()) == CastResult::NoComboPoints);
  CHECK(
      ambush.Rogue.CastSpell(SPELL_KIDNEY_SHOT, ambush.Victim.GetId()) ==
      CastResult::NoComboPoints);

  ambush.Build(1);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_EVISCERATE, ambush.Victim.GetId()) == CastResult::Ok);
  std::vector<CombatEvent> damage = ambush.Take(CombatEventType::Damage);
  REQUIRE(damage.size() == 1);
  CHECK(damage[0].Amount >= 6 + 18);
  CHECK(damage[0].Amount <= 10 + 18);
  CHECK(ambush.Rogue.GetComboPoints() == 0);
  ambush.Wait();

  ambush.Victim.Revive();
  ambush.Build(5);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_EVISCERATE, ambush.Victim.GetId()) == CastResult::Ok);
  damage = ambush.Take(CombatEventType::Damage);
  REQUIRE(damage.size() == 1);
  CHECK(damage[0].Amount >= 6 + 5 * 18);
  CHECK(damage[0].Amount <= 10 + 5 * 18);
}

TEST_CASE("Kidney Shot stuns a second a combo point", "[rogue]") {
  Ambush ambush;
  ambush.Build(3);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_KIDNEY_SHOT, ambush.Victim.GetId()) == CastResult::Ok);
  std::vector<CombatEvent> applied = ambush.Take(CombatEventType::AuraApplied);
  REQUIRE(applied.size() == 1);
  CHECK(applied[0].Spell == AURA_KIDNEY_SHOT);
  CHECK(applied[0].Duration == 3000);
  CHECK(ambush.Victim.IsStunned());
  CHECK(ambush.Rogue.GetCooldown(SPELL_KIDNEY_SHOT) == 20000);

  ambush.Victim.Update(2999);
  CHECK(ambush.Victim.IsStunned());
  ambush.Victim.Update(1);
  CHECK(!ambush.Victim.IsStunned());
}

TEST_CASE("Sprint: 70% faster for 4 s, off the global cooldown", "[rogue]") {
  Ambush ambush;
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SPRINT, ambush.Rogue.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.HasAura(AURA_SPRINT));
  CHECK(ambush.Rogue.GetSpeedMultiplier() == Catch::Approx(1.7f));
  CHECK(ambush.Rogue.CastSpell(SPELL_SPRINT, ambush.Rogue.GetId()) == CastResult::NotReady);

  ambush.Rogue.Update(4000);
  CHECK(ambush.Rogue.GetSpeedMultiplier() == Catch::Approx(1.0f));
}

TEST_CASE("Shadowstep: behind the target, facing it, within 300", "[rogue]") {
  Ambush ambush;
  // The victim at 500,300 looking along +y (down the field): behind it is up, smaller y
  ambush.Victim.Relocate(500.0f, 300.0f);
  ambush.Victim.SetFacing(float(M_PI) / 2.0f);
  ambush.Rogue.Relocate(500.0f - 2 * Unit::Radius - 301.0f, 300.0f);
  CHECK(ambush.Rogue.CastSpell(SPELL_SHADOWSTEP, ambush.Victim.GetId()) == CastResult::OutOfRange);

  ambush.Rogue.Relocate(500.0f - 2 * Unit::Radius - 300.0f, 300.0f);
  REQUIRE(
      ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) ==
      CastResult::OutOfRange);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SHADOWSTEP, ambush.Victim.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.GetX() == Catch::Approx(500.0f).margin(0.01f));
  CHECK(ambush.Rogue.GetY() == Catch::Approx(300.0f - 2 * Unit::Radius - 1.0f));
  CHECK(ambush.Rogue.GetFacing() == Catch::Approx(float(M_PI) / 2.0f));
  CHECK(ambush.Rogue.GetDistance(ambush.Victim) <= Unit::MeleeRange);
  CHECK(ambush.Rogue.GetCooldown(SPELL_SHADOWSTEP) == 15000);
  // Off the global cooldown, and in reach now
  CHECK(ambush.Rogue.CastSpell(SPELL_SINISTER_STRIKE, ambush.Victim.GetId()) == CastResult::Ok);
}

TEST_CASE("Shadowstep stays on the field", "[rogue]") {
  Ambush ambush;
  // Behind a victim at the top edge, looking down, would be off the field
  ambush.Victim.Relocate(500.0f, Unit::Radius);
  ambush.Victim.SetFacing(float(M_PI) / 2.0f);
  ambush.Rogue.Relocate(400.0f, 200.0f);
  REQUIRE(ambush.Rogue.CastSpell(SPELL_SHADOWSTEP, ambush.Victim.GetId()) == CastResult::Ok);
  CHECK(ambush.Rogue.GetY() >= Unit::Radius);
}

TEST_CASE("A rogue swings every 1.5 s; dying loses the combo points", "[rogue]") {
  Ambush ambush;
  REQUIRE(ambush.Rogue.StartAttack(ambush.Victim.GetId()) == CastResult::Ok);
  ambush.Wait(1);
  CHECK(ambush.Take(CombatEventType::Swing).size() == 1);
  ambush.Wait(1499);
  CHECK(ambush.Take(CombatEventType::Swing).empty());
  ambush.Wait(1);
  CHECK(ambush.Take(CombatEventType::Swing).size() == 1);

  ambush.Rogue.StopAttack();
  ambush.Build(2);
  ambush.Rogue.TakeDamage(2, 0, ambush.Rogue.GetHealth(), false, false);
  CHECK(ambush.Rogue.GetComboPoints() == 0);
}
