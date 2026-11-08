/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "TaskScheduler.h"
#include <utility>

void TaskScheduler::Schedule(Milliseconds delay, Group group, Task task)
{
    Add(_now + delay, { .InGroup = group, .Interval = delay, .Repeated = 0, .Run = std::make_shared<Task>(std::move(task)) });
}

void TaskScheduler::Add(Milliseconds due, Entry entry)
{
    _due.emplace(due, std::move(entry));
}

void TaskScheduler::CancelAll()
{
    _due.clear();
    _next.clear();
}

void TaskScheduler::CancelGroup(Group group)
{
    std::erase_if(_due, [group](auto const& entry) { return entry.second.InGroup == group; });
    std::erase_if(_next, [group](Entry const& entry) { return entry.InGroup == group; });
}

void TaskScheduler::RescheduleGroup(Group group, Milliseconds delay)
{
    std::vector<Entry> moved;
    for (auto itr = _due.begin(); itr != _due.end();)
    {
        if (itr->second.InGroup == group)
        {
            moved.push_back(std::move(itr->second));
            itr = _due.erase(itr);
        }
        else
            ++itr;
    }
    std::erase_if(_next, [&](Entry& entry)
    {
        if (entry.InGroup != group)
            return false;
        moved.push_back(std::move(entry));
        return true;
    });
    for (Entry& entry : moved)
        Add(_now + delay, std::move(entry));
}

void TaskScheduler::Update(Milliseconds elapsed)
{
    for (Entry& entry : std::exchange(_next, {}))
        Add(_now, std::move(entry));

    Milliseconds until = _now + elapsed;
    _updating = true;
    while (!_due.empty() && _due.begin()->first <= until)
    {
        auto first = _due.begin();
        Milliseconds due = first->first;
        Entry entry = std::move(first->second);
        _due.erase(first);

        // The task's own time: what it schedules or repeats counts from there
        _now = due;
        Task& run = *entry.Run;
        run(TaskContext(*this, due, std::move(entry)));
    }
    _updating = false;
    _now = until;
}

void TaskContext::Repeat()
{
    Repeat(_entry.Interval);
}

void TaskContext::Repeat(Milliseconds delay)
{
    TaskScheduler::Entry entry = _entry;
    entry.Interval = delay;
    ++entry.Repeated;
    if (delay <= 0ms && _scheduler->_updating)
        _scheduler->_next.push_back(std::move(entry));
    else
        _scheduler->Add(_due + delay, std::move(entry));
}
