/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "UpdateLoop.h"
#include "Errors.h"
#include "IoContext.h"
#include "Log.h"
#include "ServerSession.h"
#include "Timer.h"
#include <thread>

namespace Rooftree::Server
{
void UpdateLoop::Run(std::function<void(uint32 diff)> const& update)
{
    uint32 realPrevTime = getMSTime();
    TimePoint lastStatsLog = TimePoint::clock::now();
    _stats = {};

    while (!IsStopped())
    {
        _loopCounter.fetch_add(1, std::memory_order_relaxed);
        uint32 realCurrTime = getMSTime();

        uint32 diff = getMSTimeDiff(realPrevTime, realCurrTime);
        uint32 minUpdateDiff = uint32(_minUpdateDiff.count());
        if (diff < minUpdateDiff)
        {
            // sleep until enough time passes that we can update all timers
            std::this_thread::sleep_for(Milliseconds(minUpdateDiff - diff));
            continue;
        }

        TimePoint updateStart = TimePoint::clock::now();
        // Rooftree: one update that throws is logged, the next one runs; the server stays up
        try
        {
            update(diff);
        }
        catch (...)
        {
            RT_LOG_ERROR("server.world", "An update threw: {}", DescribeException(std::current_exception()));
        }
        TimePoint updateEnd = TimePoint::clock::now();
        realPrevTime = realCurrTime;

        std::chrono::microseconds updateTime = std::chrono::duration_cast<std::chrono::microseconds>(updateEnd - updateStart);
        ++_stats.Updates;
        _stats.TotalUpdateTime += updateTime;
        _stats.MaxUpdateTime = std::max(_stats.MaxUpdateTime, updateTime);
        _stats.MaxDiff = std::max(_stats.MaxDiff, diff);

        if (_stopAt && updateEnd >= *_stopAt)
        {
            RT_LOG_INFO("server.world", "Stopping as scheduled, exit code {}", _scheduledExitCode);
            _exitCode = _scheduledExitCode;
            _stopAt.reset();
            Stop();
        }

        if (_statsInterval > 0ms && updateEnd - lastStatsLog >= _statsInterval)
        {
            LogStats();
            lastStatsLog = updateEnd;
            _stats = {};
        }
    }
}

void UpdateLoop::StopAfter(Milliseconds delay, int exitCode)
{
    _stopAt = TimePoint::clock::now() + delay;
    _scheduledExitCode = exitCode;
}

bool UpdateLoop::CancelScheduledStop()
{
    return std::exchange(_stopAt, std::nullopt).has_value();
}

void UpdateLoop::LogStats()
{
    if (!_stats.Updates)
        return;

    RT_LOG_INFO("server.world", "{} updates, update time avg {} us max {} us, longest diff {} ms",
        _stats.Updates, _stats.TotalUpdateTime.count() / _stats.Updates, _stats.MaxUpdateTime.count(), _stats.MaxDiff);
}

std::shared_ptr<FreezeDetector> FreezeDetector::Start(Asio::IoContext& ioContext, UpdateLoop const& loop, Milliseconds maxCoreStuckTime)
{
    std::shared_ptr<FreezeDetector> detector = std::make_shared<FreezeDetector>(ioContext, loop, maxCoreStuckTime);
    detector->Schedule(detector, 5s);
    return detector;
}

FreezeDetector::FreezeDetector(Asio::IoContext& ioContext, UpdateLoop const& loop, Milliseconds maxCoreStuckTime)
    : _timer(ioContext), _loop(loop), _loopCounter(loop.GetLoopCounter()), _lastChangeMsTime(getMSTime()),
    _maxCoreStuckTimeInMs(uint32(maxCoreStuckTime.count()))
{
}

void FreezeDetector::Schedule(std::weak_ptr<FreezeDetector> self, Milliseconds delay)
{
    _timer.expires_after(delay);
    _timer.async_wait([self = std::move(self)](boost::system::error_code const& error) mutable
    {
        Handler(std::move(self), error);
    });
}

void FreezeDetector::Handler(std::weak_ptr<FreezeDetector> freezeDetectorRef, boost::system::error_code const& error)
{
    if (error)
        return;

    std::shared_ptr<FreezeDetector> freezeDetector = freezeDetectorRef.lock();
    if (!freezeDetector)
        return;

    uint32 curtime = getMSTime();
    uint32 loopCounter = freezeDetector->_loop.GetLoopCounter();
    if (freezeDetector->_loopCounter != loopCounter)
    {
        freezeDetector->_lastChangeMsTime = curtime;
        freezeDetector->_loopCounter = loopCounter;
    }
    // possible freeze
    else
    {
        uint32 msTimeDiff = getMSTimeDiff(freezeDetector->_lastChangeMsTime, curtime);
        if (msTimeDiff > freezeDetector->_maxCoreStuckTimeInMs)
        {
            RT_LOG_FATAL("server.world", "Main thread hangs for {} ms, forcing a crash!", msTimeDiff);
            ABORT_MSG("Main thread hangs for %u ms, forcing a crash!", msTimeDiff);
        }
    }

    freezeDetector->Schedule(std::move(freezeDetectorRef), 1s);
}
}
