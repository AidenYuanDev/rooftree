/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Analytics.h"
#include "BlogDatabase.h"
#include "PreparedStatement.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <chrono>
#include <functional>

namespace Blog
{
std::string Analytics::DayOf(SystemTimePoint time) const
{
    std::chrono::year_month_day date(std::chrono::floor<std::chrono::days>(time + _dayOffset));
    return Rooftree::StringFormat("{:04}-{:02}-{:02}", int(date.year()), unsigned(date.month()), unsigned(date.day()));
}

void Analytics::Count(std::string const& path, std::string const& referrer, std::string_view visitor, SystemTimePoint now)
{
    std::string day = DayOf(now);
    if (day != _pending.Day)
    {
        // Yesterday's sums go under yesterday; who came is forgotten with its salt
        Flush();
        _pending.Day = std::move(day);
        _salt = Rooftree::Server::GenerateToken(16);
        _seen.clear();
    }

    std::string hash = Rooftree::Server::Sha256Hex(_salt + "|" + std::string(visitor));
    uint64 key = std::stoull(hash.substr(0, 16), nullptr, 16);
    uint64 pageKey = key ^ std::hash<std::string>()(path);

    Pending::Counts& site = _pending.Pages[""];
    ++site.Views;
    if (_seen.insert(key).second)
        ++site.Visitors;
    Pending::Counts& page = _pending.Pages[path];
    ++page.Views;
    if (_seen.insert(pageKey).second)
        ++page.Visitors;
    if (!referrer.empty())
        ++_pending.Referrers[referrer];
}

Analytics::Pending Analytics::Take()
{
    Pending taken{ .Day = _pending.Day };
    std::swap(taken.Pages, _pending.Pages);
    std::swap(taken.Referrers, _pending.Referrers);
    return taken;
}

void Analytics::Write(Pending const& pending, bool now)
{
    auto run = [now](BlogDatabasePreparedStatement* stmt)
    {
        if (now)
            BlogDatabase.DirectExecute(stmt);
        else
            BlogDatabase.Execute(stmt);
    };
    for (auto const& [path, counts] : pending.Pages)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_PAGE_VIEWS);
        stmt->setString(0, pending.Day);
        stmt->setString(1, path);
        stmt->setUInt32(2, counts.Views);
        stmt->setUInt32(3, counts.Visitors);
        run(stmt);
    }
    for (auto const& [host, views] : pending.Referrers)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_PAGE_REFERRER);
        stmt->setString(0, pending.Day);
        stmt->setString(1, host);
        stmt->setUInt32(2, views);
        run(stmt);
    }
}
}
