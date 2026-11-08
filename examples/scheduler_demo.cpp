/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * scheduler_demo: the two tick-driven schedulers, as a game's world update uses them.
 *
 * Neither owns a thread or a timer. Something has to call Update(diff) every tick, like the
 * arena's world tick does.
 *
 *   TaskScheduler  - lambdas with Repeat and groups
 *   EventMap       - numbered events you poll with ExecuteEvent() (a boss script)
 */

#include "Config.h"
#include "EventMap.h"
#include "Log.h"
#include "TaskScheduler.h"
#include <thread>

using namespace std::chrono_literals;

enum BossEvents : uint32
{
    EVENT_FIREBALL = 1,
    EVENT_ENRAGE   = 2,
};

int main(int argc, char** argv)
{
    std::string configFile = argc > 1 ? argv[1] : "rooftree.yaml";
    std::string configError;
    if (!sConfigMgr->LoadFile(configFile, configError))
        fmt::print(stderr, "Could not load {} ({}), using defaults\n", configFile, configError);

    sLog->LoadFromConfig();

    TaskScheduler scheduler;
    scheduler.Schedule(200ms, [](TaskContext context)
    {
        RT_LOG_INFO("demo.tasks", "[TaskScheduler] heartbeat #{}", context.GetRepeatCounter());
        if (context.GetRepeatCounter() < 9)
            context.Repeat();
    });
    scheduler.Schedule(1s, [](TaskContext /*context*/)
    {
        RT_LOG_INFO("demo.tasks", "[TaskScheduler] one-shot after 1s");
    });

    EventMap events;
    events.ScheduleEvent(EVENT_FIREBALL, 300ms);
    events.ScheduleEvent(EVENT_ENRAGE, 1500ms);

    // Fixed 50ms tick, like a game server's world update loop
    constexpr std::chrono::milliseconds tick = 50ms;
    for (int i = 0; i < 2000ms / tick; ++i)
    {
        scheduler.Update(tick);

        events.Update(tick);
        while (uint32 eventId = events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_FIREBALL:
                    RT_LOG_INFO("demo.events", "[EventMap] Fireball!");
                    events.Repeat(400ms);
                    break;
                case EVENT_ENRAGE:
                    RT_LOG_INFO("demo.events", "[EventMap] Enrage, fireballs stop");
                    events.CancelEvent(EVENT_FIREBALL);
                    break;
                default:
                    break;
            }
        }

        std::this_thread::sleep_for(tick);
    }

    return 0;
}
