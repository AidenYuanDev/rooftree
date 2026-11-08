/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Numbered events due after a while, for a loop that asks what is due (a boss script, the arena's
 * gold rush):
 *
 *   events.ScheduleEvent(EVENT_RUSH_START, 5min);
 *   ...
 *   events.Update(diff);
 *   while (uint16 id = events.ExecuteEvent())
 *       switch (id) { case EVENT_RUSH_START: ...; events.Repeat(5min); break; }
 *
 * An event may belong to a group (1 to 8), to cancel or delay several at once, and to a phase (1 to 8):
 * one of a phase waits while the map is in other phases only. Time is the sum of what Update was given.
 */

#ifndef ROOFTREE_EVENT_MAP_H
#define ROOFTREE_EVENT_MAP_H

#include "Define.h"
#include "Duration.h"
#include <map>

class EventMap
{
public:
    void Reset();
    void Update(uint32 diff) { Update(Milliseconds(diff)); }
    void Update(Milliseconds elapsed) { _now += elapsed; }

    // A second one of the same id is another event; RescheduleEvent replaces it
    void ScheduleEvent(uint16 id, Milliseconds delay, uint8 group = 0, uint8 phase = 0);
    void RescheduleEvent(uint16 id, Milliseconds delay, uint8 group = 0, uint8 phase = 0);
    void CancelEvent(uint16 id);
    void CancelEventGroup(uint8 group);
    // Everything, or a group's, later by delay
    void DelayEvents(Milliseconds delay, uint8 group = 0);

    // The id of the first event due in the current phase, taken out of the map; 0 for none
    uint16 ExecuteEvent();
    // The event ExecuteEvent last gave, again after delay
    void Repeat(Milliseconds delay);

    // Phases 1 to 8, several at once; phase 0 is none
    void SetPhase(uint8 phase) { _phases = PhaseMask(phase); }
    void AddPhase(uint8 phase) { _phases |= PhaseMask(phase); }
    void RemovePhase(uint8 phase) { _phases &= uint8(~PhaseMask(phase)); }
    bool IsInPhase(uint8 phase) const { return phase && (_phases & PhaseMask(phase)); }

    // How long until the first event of id, zero if it is due, Milliseconds::max() without one
    Milliseconds GetTimeUntilEvent(uint16 id) const;
    bool Empty() const { return _events.empty(); }

private:
    struct Event
    {
        uint16 Id;
        uint8 Groups;
        uint8 Phases;
    };

    static uint8 PhaseMask(uint8 phase) { return phase ? uint8(1u << (phase - 1)) : 0; }
    bool CanRun(Event const& event) const { return !_phases || !event.Phases || (event.Phases & _phases); }

    Milliseconds _now{};
    std::multimap<Milliseconds, Event> _events;    // by when
    uint8 _phases = 0;
    Event _last{};                                  // what ExecuteEvent gave last
};

#endif // ROOFTREE_EVENT_MAP_H
