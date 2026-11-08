/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Reads of the site's pages, counted on the main thread as the readers' pages report them
 * (POST /api/views) and written once a minute as sums per page and day (page_view, page_referrer).
 * A reader is known within a day only, by a hash of their address and browser with a salt drawn
 * anew each day and never written: the counts say how many, never who.
 *
 * The day begins at midnight Blog.Stats.DayOffset minutes east of UTC. A restart forgets who was
 * counted that day: readers who come back after it count as visitors again.
 */

#ifndef BLOG_ANALYTICS_H
#define BLOG_ANALYTICS_H

#include "Define.h"
#include "Duration.h"
#include <map>
#include <string>
#include <unordered_set>

namespace Blog
{
class Analytics
{
public:
    // A page read: path ("/zh/posts/hello"), the other site the reader came from (a host, or empty),
    // who they are (address and browser, hashed here)
    void Count(std::string const& path, std::string const& referrer, std::string_view visitor, SystemTimePoint now);

    // The sums since the last write, of one day
    struct Pending
    {
        struct Counts
        {
            uint32 Views = 0;
            uint32 Visitors = 0;
        };

        std::string Day;
        std::map<std::string, Counts> Pages;    // "": the whole site
        std::map<std::string, uint32> Referrers;
    };

    // The sums since the last write, taken to be written
    Pending Take();
    // Into the database: queued (main thread, never blocks), or right away (a worker, before reading)
    static void Write(Pending const& pending, bool now);
    void Flush() { Write(Take(), false); }

    void SetDayOffset(Minutes offset) { _dayOffset = offset; }
    // "2026-10-29": the day of that time, where the stats' day is
    std::string DayOf(SystemTimePoint time) const;

private:
    Minutes _dayOffset = 0min;
    std::string _salt;
    std::unordered_set<uint64> _seen;           // a visitor of the site, and of each page, today
    Pending _pending;                           // today's, not written yet
};
}

#endif // BLOG_ANALYTICS_H
