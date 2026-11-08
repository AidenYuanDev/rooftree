/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "rooftree_catch2.h"

#include "EventMap.h"

enum : uint16 { EVENT_A = 1, EVENT_B = 2, EVENT_C = 3 };

TEST_CASE("EventMap gives what is due, in the order it is due", "[EventMap]")
{
    EventMap events;
    CHECK(events.Empty());
    CHECK(events.ExecuteEvent() == 0);

    events.ScheduleEvent(EVENT_B, 200ms);
    events.ScheduleEvent(EVENT_A, 100ms);
    events.ScheduleEvent(EVENT_C, 1s);

    events.Update(99);
    CHECK(events.ExecuteEvent() == 0);
    events.Update(150ms);
    CHECK(events.ExecuteEvent() == EVENT_A);
    CHECK(events.ExecuteEvent() == EVENT_B);
    CHECK(events.ExecuteEvent() == 0);
    CHECK_FALSE(events.Empty());
    CHECK(events.GetTimeUntilEvent(EVENT_C) == 751ms);
    CHECK(events.GetTimeUntilEvent(EVENT_A) == Milliseconds::max());
}

TEST_CASE("EventMap repeats, reschedules and cancels", "[EventMap]")
{
    EventMap events;
    events.ScheduleEvent(EVENT_A, 100ms);
    events.Update(100ms);
    REQUIRE(events.ExecuteEvent() == EVENT_A);
    // From now, the update the event was given in
    events.Repeat(50ms);
    events.Update(49ms);
    CHECK(events.ExecuteEvent() == 0);
    events.Update(1ms);
    CHECK(events.ExecuteEvent() == EVENT_A);

    // A second one of an id is another event; RescheduleEvent leaves one
    events.ScheduleEvent(EVENT_B, 10ms);
    events.ScheduleEvent(EVENT_B, 20ms);
    events.RescheduleEvent(EVENT_B, 30ms);
    events.Update(30ms);
    CHECK(events.ExecuteEvent() == EVENT_B);
    CHECK(events.ExecuteEvent() == 0);

    events.ScheduleEvent(EVENT_C, 10ms);
    events.CancelEvent(EVENT_C);
    events.CancelEvent(EVENT_A);
    events.Update(1s);
    CHECK(events.ExecuteEvent() == 0);
    CHECK(events.Empty());
}

TEST_CASE("EventMap groups are cancelled and delayed together", "[EventMap]")
{
    EventMap events;
    events.ScheduleEvent(EVENT_A, 100ms, 1);
    events.ScheduleEvent(EVENT_B, 100ms, 2);
    events.ScheduleEvent(EVENT_C, 100ms, 2);

    events.DelayEvents(50ms, 2);
    events.Update(100ms);
    CHECK(events.ExecuteEvent() == EVENT_A);
    CHECK(events.ExecuteEvent() == 0);
    CHECK(events.GetTimeUntilEvent(EVENT_B) == 50ms);

    // Without a group: all of them
    events.DelayEvents(1s);
    CHECK(events.GetTimeUntilEvent(EVENT_C) == 1050ms);

    events.CancelEventGroup(2);
    CHECK(events.Empty());
}

TEST_CASE("EventMap events of a phase wait while the map is in other phases", "[EventMap]")
{
    EventMap events;
    events.ScheduleEvent(EVENT_A, 10ms, 0, 1);
    events.ScheduleEvent(EVENT_B, 20ms, 0, 2);
    events.ScheduleEvent(EVENT_C, 30ms);
    events.Update(100ms);

    // In phase 2: phase 1's waits, one without a phase runs
    events.SetPhase(2);
    CHECK(events.IsInPhase(2));
    CHECK_FALSE(events.IsInPhase(1));
    CHECK(events.ExecuteEvent() == EVENT_B);
    CHECK(events.ExecuteEvent() == EVENT_C);
    CHECK(events.ExecuteEvent() == 0);

    events.AddPhase(1);
    CHECK(events.IsInPhase(1));
    CHECK(events.ExecuteEvent() == EVENT_A);

    // No phase at all: every event runs
    events.ScheduleEvent(EVENT_A, 0ms, 0, 3);
    events.RemovePhase(1);
    events.RemovePhase(2);
    CHECK(events.ExecuteEvent() == EVENT_A);
}

TEST_CASE("EventMap Reset forgets everything", "[EventMap]")
{
    EventMap events;
    events.ScheduleEvent(EVENT_A, 10ms);
    events.SetPhase(1);
    events.Update(5ms);
    events.Reset();
    CHECK(events.Empty());
    CHECK_FALSE(events.IsInPhase(1));
    events.ScheduleEvent(EVENT_B, 10ms);
    CHECK(events.GetTimeUntilEvent(EVENT_B) == 10ms);
}
