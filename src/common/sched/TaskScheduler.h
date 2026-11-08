/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Lambdas that run after a while, on the thread that calls Update(elapsed), the world's tick:
 *
 *   scheduler.Schedule(10min, [this](TaskContext context)
 *   {
 *       Save();
 *       context.Repeat();           // again in 10 minutes; Repeat(5min) for another interval
 *   });
 *
 * Time is the sum of what Update was given, not the clock's. A task repeats from when it was due, not
 * from when Update got to it, so one long update catches up: Update(350ms) runs a 100 ms task three
 * times. A repeat with no delay runs at the next Update, never again in the same one. A group (any
 * number but 0) lets several tasks be cancelled or moved together.
 */

#ifndef ROOFTREE_TASK_SCHEDULER_H
#define ROOFTREE_TASK_SCHEDULER_H

#include "Define.h"
#include "Duration.h"
#include <functional>
#include <map>
#include <memory>
#include <vector>

class TaskContext;

class TaskScheduler
{
public:
    using Task = std::function<void(TaskContext)>;
    using Group = uint32;

    TaskScheduler() = default;
    TaskScheduler(TaskScheduler const&) = delete;
    TaskScheduler& operator=(TaskScheduler const&) = delete;

    void Schedule(Milliseconds delay, Task task) { Schedule(delay, 0, std::move(task)); }
    void Schedule(Milliseconds delay, Group group, Task task);

    void CancelAll();
    void CancelGroup(Group group);
    // Due delay from now, whenever they were due
    void RescheduleGroup(Group group, Milliseconds delay);

    // Runs what is due within elapsed, in the order it is due
    void Update(Milliseconds elapsed);

    bool Empty() const { return _due.empty() && _next.empty(); }

private:
    friend class TaskContext;

    struct Entry
    {
        Group InGroup = 0;
        Milliseconds Interval{};        // what Repeat() repeats it with
        uint32 Repeated = 0;
        std::shared_ptr<Task> Run;      // shared with the context, which may repeat it
    };

    void Add(Milliseconds due, Entry entry);

    Milliseconds _now{};
    std::multimap<Milliseconds, Entry> _due;    // by when
    std::vector<Entry> _next;                   // repeated without a delay: the next Update's
    bool _updating = false;
};

// What a running task knows of itself
class TaskContext
{
public:
    // Again after the same delay as last time, or after delay
    void Repeat();
    void Repeat(Milliseconds delay);
    // How many times it was repeated before this run
    uint32 GetRepeatCounter() const { return _entry.Repeated; }
    TaskScheduler::Group GetGroup() const { return _entry.InGroup; }

private:
    friend class TaskScheduler;

    TaskContext(TaskScheduler& scheduler, Milliseconds due, TaskScheduler::Entry entry) : _scheduler(&scheduler), _due(due), _entry(std::move(entry)) { }

    TaskScheduler* _scheduler;
    Milliseconds _due;
    TaskScheduler::Entry _entry;
};

#endif // ROOFTREE_TASK_SCHEDULER_H
