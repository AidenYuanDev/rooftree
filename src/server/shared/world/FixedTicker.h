/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A world updated at a fixed tick inside a loop that runs more often. One loop serves the game and
 * web pages: the loop runs every MinUpdateDiff (2 ms) to answer messages, and the game's world is
 * updated every Interval (50 ms), its work never making a web request wait for a whole tick.
 *
 * Update(diff, tick) adds diff; once Interval passed it calls tick(interval), again for every further
 * Interval that passed, at most MaxCatchUp times in one call. Ticks past that are dropped and counted:
 * a world that can't keep up runs behind instead of piling up ticks that make every update longer.
 */

#ifndef ROOFTREE_FIXED_TICKER_H
#define ROOFTREE_FIXED_TICKER_H

#include "Define.h"
#include "Duration.h"
#include <algorithm>
#include <chrono>
#include <utility>

namespace Rooftree::Server
{
struct FixedTickerStats
{
    uint32 Ticks = 0;
    uint32 Dropped = 0;                     // ticks skipped, the world behind by that many
    std::chrono::microseconds TotalTickTime{};
    std::chrono::microseconds MaxTickTime{};
};

class FixedTicker
{
public:
    // 0: never ticks
    void SetInterval(Milliseconds interval) { _interval = interval; }
    Milliseconds GetInterval() const { return _interval; }
    void SetMaxCatchUp(uint32 maxCatchUp) { _maxCatchUp = std::max(1u, maxCatchUp); }

    template<typename Tick>
    void Update(uint32 diff, Tick&& tick)
    {
        if (_interval <= 0ms)
            return;

        _accumulated += Milliseconds(diff);
        uint32 ticks = 0;
        while (_accumulated >= _interval)
        {
            _accumulated -= _interval;
            if (ticks == _maxCatchUp)
            {
                ++_stats.Dropped;
                continue;
            }

            ++ticks;
            TimePoint start = TimePoint::clock::now();
            tick(uint32(_interval.count()));
            auto took = std::chrono::duration_cast<std::chrono::microseconds>(TimePoint::clock::now() - start);
            ++_stats.Ticks;
            _stats.TotalTickTime += took;
            _stats.MaxTickTime = std::max(_stats.MaxTickTime, took);
        }
    }

    // Since the last TakeStats
    FixedTickerStats TakeStats() { return std::exchange(_stats, {}); }

private:
    Milliseconds _interval = 0ms;
    Milliseconds _accumulated = 0ms;
    uint32 _maxCatchUp = 3;
    FixedTickerStats _stats;
};
}

#endif // ROOFTREE_FIXED_TICKER_H
