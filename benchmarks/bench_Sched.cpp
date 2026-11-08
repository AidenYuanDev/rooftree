/*
 * Per-tick cost of the schedulers a game loop calls every update: EventMap (a creature's AI) and
 * TaskScheduler.
 */

#include "rooftree_benchmark.h"

#include "EventMap.h"
#include "TaskScheduler.h"

using namespace std::chrono_literals;

TEST_CASE("EventMap", "[sched]")
{
    for (int events : { 10, 100, 1000 })
    {
        BENCHMARK(Bench::Name("schedule and execute", events, "events"))
        {
            EventMap map;
            for (int i = 1; i <= events; ++i)
                map.ScheduleEvent(uint16(i), Milliseconds(i % 50 * 100));
            map.Update(5000);
            int executed = 0;
            while (map.ExecuteEvent())
                ++executed;
            return executed;
        };
    }

    // A boss script: a handful of repeating abilities, Update(100) every tick, nothing due on most ticks
    EventMap boss;
    for (uint16 id = 1; id <= 8; ++id)
        boss.ScheduleEvent(id, Milliseconds(id * 1500));
    BENCHMARK("boss AI tick (8 repeating events, 100 ms)")
    {
        boss.Update(100);
        int executed = 0;
        while (uint16 id = boss.ExecuteEvent())
        {
            boss.Repeat(Milliseconds(id * 1500));
            ++executed;
        }
        return executed;
    };

    EventMap groups;
    BENCHMARK("CancelEventGroup out of 100 events in 4 groups")
    {
        for (uint16 id = 1; id <= 100; ++id)
            groups.ScheduleEvent(id, 10s, id % 4 + 1);
        groups.CancelEventGroup(2);
        groups.Reset();
        return groups.Empty();
    };
}

TEST_CASE("TaskScheduler", "[sched]")
{
    for (int tasks : { 10, 100, 1000 })
    {
        BENCHMARK(Bench::Name("schedule and run once", tasks, "tasks"))
        {
            TaskScheduler scheduler;
            int ran = 0;
            for (int i = 0; i < tasks; ++i)
                scheduler.Schedule(Milliseconds(i % 50 * 100), [&ran](TaskContext) { ++ran; });
            scheduler.Update(5s);
            return ran;
        };
    }

    TaskScheduler repeating;
    int ticks = 0;
    for (int i = 1; i <= 8; ++i)
    {
        repeating.Schedule(Milliseconds(i * 1500), [&ticks](TaskContext context)
        {
            ++ticks;
            context.Repeat();
        });
    }
    BENCHMARK("tick (8 repeating tasks, 100 ms)")
    {
        repeating.Update(100ms);
        return ticks;
    };

    TaskScheduler idle;
    idle.Schedule(24h * 365, [](TaskContext) { });
    BENCHMARK("tick with nothing due")
    {
        idle.Update(100ms);
        return 0;
    };
}
