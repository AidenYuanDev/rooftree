/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Presence.h"
#include "BlogDatabase.h"
#include "PreparedStatement.h"
#include "WorldWebSocket.h"
#include <algorithm>

namespace Blog
{
namespace
{
constexpr Milliseconds RefreshInterval = 1s;
constexpr Seconds QuietTimeout = 70s;
constexpr Milliseconds SaveInterval = 1min;
}

void Presence::Visit(std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, std::string path, uint32 userId,
    std::vector<std::string> watch)
{
    Viewer& viewer = _viewers[socket.get()];
    // A new connection where a closed one was: start over
    if (viewer.Socket.lock() != socket)
        viewer = { .Socket = socket };
    viewer.Path = std::move(path);
    viewer.UserId = userId;
    // A person, not a page: a user by their account, nobody by their address (an IPv6 one by its /64)
    viewer.Visitor = userId ? "user:" + std::to_string(userId) : socket->GetSessionKey();
    viewer.Watch = std::move(watch);
    viewer.LastSeen = TimePoint::clock::now();
}

void Presence::Changed(std::string target, std::string what)
{
    _changes.emplace(std::move(target), std::move(what));
}

void Presence::Update(uint32 diff)
{
    _sinceRefresh += Milliseconds(diff);
    if (_sinceRefresh < RefreshInterval)
        return;

    _sinceRefresh = 0ms;
    Refresh();

    _sinceSave += RefreshInterval;
    if (_sinceSave < SaveInterval)
        return;

    _sinceSave = 0ms;
    for (auto& [userId, online] : _online)
        SaveOnlineTime(userId, online, online.LastActive, false);
}

void Presence::Refresh()
{
    TimePoint now = TimePoint::clock::now();
    std::erase_if(_viewers, [now](auto const& entry)
    {
        std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = entry.second.Socket.lock();
        return !socket || !socket->IsConnected() || now - entry.second.LastSeen > QuietTimeout;
    });

    RefreshOnline(now);
    SendChanges();

    // Who, not how many pages: one reader in two tabs is one
    std::map<std::string_view, std::set<std::string_view>> perPage;
    std::set<std::string_view> visitors;
    for (auto const& [key, viewer] : _viewers)
    {
        perPage[viewer.Path].insert(viewer.Visitor);
        visitors.insert(viewer.Visitor);
    }

    _readers = visitors.size();
    int64 site = int64(visitors.size());
    for (auto& [key, viewer] : _viewers)
    {
        int64 page = int64(perPage[viewer.Path].size());
        if (page == viewer.SentPage && site == viewer.SentSite)
            continue;

        if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock())
            socket->SendJson("presence", { { "page", page }, { "site", site } });
        viewer.SentPage = page;
        viewer.SentSite = site;
    }
}

void Presence::SendChanges()
{
    if (_changes.empty())
        return;

    for (auto const& [key, viewer] : _viewers)
    {
        if (viewer.Watch.empty())
            continue;

        std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock();
        if (!socket)
            continue;

        for (auto const& [target, what] : _changes)
            if (target == "*" || std::ranges::find(viewer.Watch, target) != viewer.Watch.end())
                socket->SendJson("changed", { { "target", target }, { "what", what } });
    }
    _changes.clear();
}

void Presence::RefreshOnline(TimePoint now)
{
    std::set<uint32> present;
    for (auto const& [key, viewer] : _viewers)
    {
        if (!viewer.UserId)
            continue;

        present.insert(viewer.UserId);
        auto [itr, added] = _online.try_emplace(viewer.UserId);
        if (added)
        {
            itr->second.Since = SystemTimePoint::clock::now();
            itr->second.SavedUntil = now;
        }
        itr->second.LastActive = now;
    }

    for (auto itr = _online.begin(); itr != _online.end();)
    {
        if (present.contains(itr->first))
        {
            ++itr;
            continue;
        }

        // Online until the refresh before, the last one with a page connected
        SaveOnlineTime(itr->first, itr->second, itr->second.LastActive, false);
        itr = _online.erase(itr);
    }
}

void Presence::SaveOnlineTime(uint32 userId, Online& online, TimePoint until, bool blocking)
{
    Seconds seconds = std::chrono::duration_cast<Seconds>(until - online.SavedUntil);
    if (seconds <= 0s)
        return;

    online.SavedUntil += seconds;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_ONLINE);
    stmt->setUInt32(0, uint32(seconds.count()));
    stmt->setUInt32(1, userId);
    if (blocking)
        BlogDatabase.DirectExecute(stmt);
    else
        BlogDatabase.Execute(stmt);
}

void Presence::SaveOnlineTime()
{
    for (auto& [userId, online] : _online)
        SaveOnlineTime(userId, online, online.LastActive, true);
}

std::map<uint32, Presence::OnlineUser> Presence::GetOnlineUsers() const
{
    std::map<uint32, OnlineUser> users;
    for (auto const& [key, viewer] : _viewers)
    {
        auto online = _online.find(viewer.UserId);
        if (online == _online.end())
            continue;

        OnlineUser& user = users[viewer.UserId];
        user.Since = online->second.Since;
        user.Pages.insert(viewer.Path);
        ++user.Connections;
        user.Unsaved = std::max(0s, std::chrono::duration_cast<Seconds>(online->second.LastActive - online->second.SavedUntil));
    }
    return users;
}

Optional<Presence::OnlineUser> Presence::FindOnlineUser(uint32 userId) const
{
    std::map<uint32, OnlineUser> users = GetOnlineUsers();
    auto itr = users.find(userId);
    if (itr == users.end())
        return {};
    return std::move(itr->second);
}

void Presence::Broadcast(std::string_view type, boost::json::object const& body)
{
    for (auto const& [key, viewer] : _viewers)
        if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock())
            socket->SendJson(type, body);
}

void Presence::SendToWatchers(std::string_view target, std::string_view type, boost::json::object const& body)
{
    for (auto const& [key, viewer] : _viewers)
    {
        if (std::ranges::find(viewer.Watch, target) == viewer.Watch.end())
            continue;

        if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock())
            socket->SendJson(type, body);
    }
}

void Presence::SendToUser(uint32 userId, std::string_view type, boost::json::object const& body)
{
    if (!userId)
        return;

    for (auto const& [key, viewer] : _viewers)
    {
        if (viewer.UserId != userId)
            continue;

        if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock())
            socket->SendJson(type, body);
    }
}

void Presence::SendToLoggedIn(std::string_view type, boost::json::object const& body)
{
    for (auto const& [key, viewer] : _viewers)
    {
        if (!viewer.UserId)
            continue;

        if (std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = viewer.Socket.lock())
            socket->SendJson(type, body);
    }
}
}
