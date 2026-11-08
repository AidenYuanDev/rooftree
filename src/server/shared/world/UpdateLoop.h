/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The server's main loop and its FreezeDetector:
 *
 *  - Run() calls update(diff) as soon as at least MinUpdateDiff passed since the previous update,
 *    diff being the real time since then (1 ms by default). An idle server
 *    sleeps in between, a busy one updates back to back with bigger diffs. There is no fixed tick
 *  - every update increments a counter; FreezeDetector checks it every second on an io_context and
 *    aborts the process when it didn't change for MaxCoreStuckTime, so a supervisor (systemd)
 *    restarts a server whose main thread hangs instead of leaving it up but dead
 *  - StopAfter() stops it once a delay passed, with an exit code for main to return
 *    ("server shutdown/restart <time>"; 2 to be restarted)
 *  - every StatsInterval the loop logs how many updates ran and how long they took on
 *    average and at most ("server.world"), the numbers to tune the per-update message budget with
 */

#ifndef ROOFTREE_UPDATE_LOOP_H
#define ROOFTREE_UPDATE_LOOP_H

#include "Define.h"
#include "DeadlineTimer.h"
#include "Duration.h"
#include "Optional.h"
#include <atomic>
#include <functional>
#include <memory>

namespace Rooftree::Asio
{
class IoContext;
}

namespace Rooftree::Server
{
struct UpdateLoopStats
{
    uint32 Updates = 0;
    std::chrono::microseconds TotalUpdateTime{};
    std::chrono::microseconds MaxUpdateTime{};
    uint32 MaxDiff = 0;     // ms between two updates, at most
};

class UpdateLoop
{
public:
    explicit UpdateLoop(Milliseconds minUpdateDiff = 1ms) : _minUpdateDiff(minUpdateDiff) { }

    // Main thread, also between updates (config reload)
    void SetMinUpdateDiff(Milliseconds minUpdateDiff) { _minUpdateDiff = minUpdateDiff; }
    void SetStatsInterval(Milliseconds interval) { _statsInterval = interval; }   // 0: no stats log

    // Until Stop()
    void Run(std::function<void(uint32 diff)> const& update);

    // Main thread: Stop() once delay passed, unless cancelled. Another call replaces it
    void StopAfter(Milliseconds delay, int exitCode = 0);
    bool CancelScheduledStop();             // false: none was scheduled
    Optional<TimePoint> GetScheduledStop() const { return _stopAt; }
    // What main returns: the scheduled stop's, else 0
    int GetExitCode() const { return _exitCode; }

    // Any thread
    void Stop() { _stopped = true; }
    bool IsStopped() const { return _stopped; }
    uint32 GetLoopCounter() const { return _loopCounter.load(std::memory_order_relaxed); }

    // Main thread: the stats since the last log (or since Run() without a stats interval)
    UpdateLoopStats const& GetStats() const { return _stats; }

private:
    void LogStats();

    Milliseconds _minUpdateDiff;
    Milliseconds _statsInterval = 60s;
    std::atomic<bool> _stopped = false;
    std::atomic<uint32> _loopCounter = 0;
    UpdateLoopStats _stats;
    Optional<TimePoint> _stopAt;
    int _scheduledExitCode = 0;
    int _exitCode = 0;
};

class FreezeDetector
{
public:
    // Checks loop every second on ioContext, aborts after maxCoreStuckTime without an update.
    // Keep the returned pointer, the detector stops when it is destroyed
    static std::shared_ptr<FreezeDetector> Start(Asio::IoContext& ioContext, UpdateLoop const& loop, Milliseconds maxCoreStuckTime);

    FreezeDetector(Asio::IoContext& ioContext, UpdateLoop const& loop, Milliseconds maxCoreStuckTime);

private:
    static void Handler(std::weak_ptr<FreezeDetector> freezeDetectorRef, boost::system::error_code const& error);
    void Schedule(std::weak_ptr<FreezeDetector> self, Milliseconds delay);

    Asio::DeadlineTimer _timer;
    UpdateLoop const& _loop;
    uint32 _loopCounter;
    uint32 _lastChangeMsTime;
    uint32 _maxCoreStuckTimeInMs;
};
}

#endif // ROOFTREE_UPDATE_LOOP_H
