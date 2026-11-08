/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "EventMap.h"
#include "Errors.h"
#include <vector>

void EventMap::Reset()
{
    _events.clear();
    _now = {};
    _phases = 0;
    _last = {};
}

void EventMap::ScheduleEvent(uint16 id, Milliseconds delay, uint8 group, uint8 phase)
{
    ASSERT(id, "EventMap: 0 is no event");
    ASSERT(group <= 8 && phase <= 8, "EventMap: groups and phases are 1 to 8");
    _events.emplace(_now + delay, Event{ .Id = id, .Groups = PhaseMask(group), .Phases = PhaseMask(phase) });
}

void EventMap::RescheduleEvent(uint16 id, Milliseconds delay, uint8 group, uint8 phase)
{
    CancelEvent(id);
    ScheduleEvent(id, delay, group, phase);
}

void EventMap::CancelEvent(uint16 id)
{
    std::erase_if(_events, [id](auto const& event) { return event.second.Id == id; });
}

void EventMap::CancelEventGroup(uint8 group)
{
    uint8 mask = PhaseMask(group);
    std::erase_if(_events, [mask](auto const& event) { return event.second.Groups & mask; });
}

void EventMap::DelayEvents(Milliseconds delay, uint8 group)
{
    uint8 mask = PhaseMask(group);
    std::vector<std::pair<Milliseconds, Event>> delayed;
    for (auto itr = _events.begin(); itr != _events.end();)
    {
        if (!mask || (itr->second.Groups & mask))
        {
            delayed.emplace_back(itr->first + delay, itr->second);
            itr = _events.erase(itr);
        }
        else
            ++itr;
    }
    _events.insert(delayed.begin(), delayed.end());
}

uint16 EventMap::ExecuteEvent()
{
    for (auto itr = _events.begin(); itr != _events.end() && itr->first <= _now; ++itr)
    {
        if (!CanRun(itr->second))
            continue;
        _last = itr->second;
        _events.erase(itr);
        return _last.Id;
    }
    return 0;
}

void EventMap::Repeat(Milliseconds delay)
{
    if (_last.Id)
        _events.emplace(_now + delay, _last);
}

Milliseconds EventMap::GetTimeUntilEvent(uint16 id) const
{
    for (auto const& [due, event] : _events)
        if (event.Id == id)
            return due > _now ? due - _now : Milliseconds::zero();
    return Milliseconds::max();
}
