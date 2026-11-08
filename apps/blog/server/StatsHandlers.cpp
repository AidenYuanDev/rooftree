/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Reads of the site (Analytics.h): POST /api/views from the readers' pages, the admin's page of them
 * (GET /api/admin/stats). Only pages of the site count (the prerendered ones, when there is a site):
 * not the drive, the account's pages or the admin's, not what bots fetch.
 */

#include "Analytics.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "StaticSite.h"
#include "StringConvert.h"
#include "Validation.h"
#include <algorithm>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr std::size_t MaxPathLength = 255;
constexpr uint32 MaxStatsDays = 366;

// Pages of one reader, never counted
constexpr std::string_view PrivatePages[] = { "admin", "drive", "s", "account", "login", "register", "forgot-password",
    "reset-password", "verify-email", "subscribe-confirm", "unsubscribe" };

// What a browser sends of a crawler, a preview or a script, lowercase
constexpr std::string_view BotMarks[] = { "bot", "crawl", "spider", "slurp", "headless", "lighthouse", "preview", "curl", "wget",
    "python", "java/", "go-http", "okhttp", "facebookexternalhit", "monitor" };

bool IsBot(std::string_view userAgent)
{
    if (userAgent.empty())
        return true;
    std::string lower(userAgent);
    std::ranges::transform(lower, lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return std::ranges::any_of(BotMarks, [&](std::string_view mark) { return lower.find(mark) != std::string::npos; });
}

// "/zh/posts/hello" as it is counted (no trailing /), empty when it isn't a page of the site
std::string PagePath(std::string_view path)
{
    if (path.size() > MaxPathLength || !(path.starts_with("/zh") || path.starts_with("/en")))
        return {};
    if (path.size() > 3 && path[3] != '/')
        return {};
    if (path.find_first_of("?#\\") != std::string_view::npos || !IsCleanText(path, false) || path.find("//") != std::string_view::npos
        || path.find("/..") != std::string_view::npos)
        return {};
    while (path.size() > 3 && path.ends_with('/'))
        path.remove_suffix(1);

    std::string_view rest = path.size() > 4 ? path.substr(4) : std::string_view();
    std::string_view section = rest.substr(0, rest.find('/'));
    if (std::ranges::find(PrivatePages, section) != std::end(PrivatePages))
        return {};
    return std::string(path);
}

// The host of a referrer URL, lowercase; empty when it is none, or ours
std::string ReferrerHost(std::string_view url, std::string_view ownOrigin)
{
    std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos || (url.substr(0, scheme) != "http" && url.substr(0, scheme) != "https"))
        return {};
    std::string_view rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (std::size_t at = rest.rfind('@'); at != std::string_view::npos)
        rest = rest.substr(at + 1);
    rest = rest.substr(0, rest.find(':'));
    std::string host(rest);
    std::ranges::transform(host, host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (host.empty() || host.size() > 253 || !std::ranges::all_of(host, [](char c) { return std::isalnum((unsigned char)c) || c == '.' || c == '-'; }))
        return {};

    std::string_view own = ownOrigin.substr(ownOrigin.find("://") == std::string_view::npos ? 0 : ownOrigin.find("://") + 3);
    own = own.substr(0, own.find_first_of(":/"));
    return host == own ? std::string() : host;
}
}

// {path, referrer?}: a page read. Always ok, counted or not (a bot learns nothing)
Rooftree::Task<void> CountView(Session& /*session*/, ApiRequest request)
{
    Reader::ViewRequest const& in = request->Get<Reader::ViewRequest>();
    std::string path = in.path() ? PagePath(in.path()->string_view()) : std::string();
    std::string const& userAgent = request->GetUserAgent();
    if (path.empty() || IsBot(userAgent))
        co_return RespondOk(request);

    // With a site, only its pages: an address made up isn't a row
    if (std::shared_ptr<Rooftree::Server::StaticSite const> site = sWorld->GetStaticSite().Get())
        if (!site->Find(path + "/index.html") && !site->Find(path + ".html"))
            co_return RespondOk(request);

    std::string referrer = in.referrer() && in.referrer()->size() <= 2048
        ? ReferrerHost(in.referrer()->string_view(), sWorld->GetSettings().PublicOrigin) : std::string();
    sWorld->GetAnalytics().Count(path, referrer, request->GetClientAddress().to_string() + "|" + userAgent, SystemTimePoint::clock::now());
    RespondOk(request);
}

// ?days=30 (at most 366): {days: [{day, views, visitors}] (every day, 0 when none), pages: [{path,
// views, visitors}] (the 50 most read), referrers: [{host, views}] (the 30 most), today}. Visitors
// summed over days count a reader once a day
Rooftree::Task<void> AdminStats(Session& session, ApiRequest request)
{
    uint32 days = std::clamp<uint32>(request->Get<Admin::StatsRequest>().days().value_or(30), 1, MaxStatsDays);
    Analytics& analytics = sWorld->GetAnalytics();
    SystemTimePoint now = SystemTimePoint::clock::now();
    std::vector<std::string> range;
    for (uint32 i = days; i-- > 0;)
        range.push_back(analytics.DayOf(now - std::chrono::days(i)));

    // What was counted is in the database before it is read
    std::string payload = co_await session.OnWorker([range, pending = analytics.Take()]
    {
        Analytics::Write(pending, true);
        std::map<std::string, std::pair<uint32, uint32>> byDay;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_STATS_DAYS);
        stmt->setString(0, range.front());
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                byDay[fields[0].GetString()] = { fields[1].GetUInt32(), fields[2].GetUInt32() };
            }
            while (rows->NextRow());
        }
        // Built here, the answer is all the worker's
        flatbuffers::FlatBufferBuilder builder;
        std::vector<flatbuffers::Offset<Admin::StatsDay>> daysTable;
        for (std::string const& day : range)
        {
            auto [views, visitors] = byDay.contains(day) ? byDay[day] : std::pair<uint32, uint32>();
            daysTable.push_back(Admin::CreateStatsDay(builder, builder.CreateString(day), views, visitors));
        }

        std::vector<flatbuffers::Offset<Admin::StatsPage>> pages;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_STATS_PAGES);
        stmt->setString(0, range.front());
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                pages.push_back(Admin::CreateStatsPage(builder, builder.CreateString(fields[0].GetString()), fields[1].GetUInt64(), fields[2].GetUInt64()));
            }
            while (rows->NextRow());
        }

        std::vector<flatbuffers::Offset<Admin::StatsReferrer>> referrers;
        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_STATS_REFERRERS);
        stmt->setString(0, range.front());
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                referrers.push_back(Admin::CreateStatsReferrer(builder, builder.CreateString(fields[0].GetString()), fields[1].GetUInt64()));
            }
            while (rows->NextRow());
        }
        builder.Finish(Admin::CreateStats(builder, builder.CreateVector(daysTable), builder.CreateVector(pages), builder.CreateVector(referrers),
            builder.CreateString(range.back())));
        return Bytes(builder);
    });
    request->Respond(std::move(payload));
}
}
