/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "CombatMap.h"
#include "Errors.h"
#include "Unit.h"
#include <algorithm>
#include <utility>

CombatMap::CombatMap() : _random(std::random_device()())
{
}

CombatMap::CombatMap(uint32 seed) : _random(seed)
{
}

CombatMap::~CombatMap()
{
    for (auto& [id, unit] : _units)
        unit->_map = nullptr;
}

void CombatMap::AddUnit(Unit& unit)
{
    auto [itr, inserted] = _units.try_emplace(unit.GetId(), &unit);
    ASSERT(itr->second == &unit, "unit %u is in the map twice", unit.GetId());
    ASSERT(!unit._map || unit._map == this, "unit %u is in another map", unit.GetId());
    unit._map = this;
}

void CombatMap::RemoveUnit(Unit& unit)
{
    if (unit._map != this)
        return;

    // Casts at it fail when they end (TargetLost), its auras on others run out as they would
    _units.erase(unit.GetId());
    unit._map = nullptr;
}

Unit* CombatMap::FindUnit(uint32 id) const
{
    auto itr = _units.find(id);
    return itr != _units.end() ? itr->second : nullptr;
}

void CombatMap::Update(uint32 diff)
{
    // A unit's update changes others (damage, death), never which units there are
    for (auto& [id, unit] : _units)
        unit->Update(diff);
}

std::vector<CombatEvent> CombatMap::TakeEvents()
{
    return std::exchange(_events, {});
}

void CombatMap::ClampToField(float& x, float& y, float radius) const
{
    if (_width > 2 * radius && _height > 2 * radius)
    {
        x = std::clamp(x, radius, _width - radius);
        y = std::clamp(y, radius, _height - radius);
    }
}

uint32 CombatMap::Roll(uint32 min, uint32 max)
{
    return std::uniform_int_distribution<uint32>(min, max)(_random);
}
