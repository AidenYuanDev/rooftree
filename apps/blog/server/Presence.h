/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Who is reading what, from the browsers' WebSockets (BlogWebSocket.h): each open page says which
 * page it shows ("page", again every 25 seconds), the main thread counts readers per page, and once a
 * second tells every connection the counts that changed for it: {"type": "presence", "page": 2,
 * "site": 9}. Readers, not pages: a logged in one counts once by their account, the others once by
 * their address. Connections that went quiet for 70 seconds, or closed, are forgotten.
 *
 * Also the way to tell all readers something at once: Broadcast (a new post was published).
 *
 * And what a page shows that others change: with its path a page names what it watches ("watch":
 * ["post:12"], a post's text and its comments, "guestbook"), and Changed tells the pages watching it
 * what changed of it, once at the next refresh however often it changed in that second: {"type":
 * "changed", "target": "post:12", "what": "comments"} ("reactions", "content"). The page asks the API
 * again, as whoever reads it; nothing it may not see is in the message. Target "*": every page that
 * watches anything (an account's comments went at once).
 *
 * And who of the logged in users is online, for the admin: a user is online while a page of theirs is
 * connected, from the first page on until the last one closed or went quiet. The time online is added
 * to user.online_seconds (with last_seen_at) once a minute and when they leave, so a crash loses at
 * most a minute of it.
 *
 * Main thread only, like everything in BlogWorld; the sends go out on the network threads.
 */

#ifndef BLOG_PRESENCE_H
#define BLOG_PRESENCE_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <boost/json/object.hpp>
#include <map>
#include <set>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Rooftree::Server
{
class WorldWebSocket;
}

namespace Blog
{
class Presence
{
public:
    // A connection shows path ("/zh/posts/hello"; the language is part of the page), logged in as
    // userId (0: nobody), watching those targets
    void Visit(std::shared_ptr<Rooftree::Server::WorldWebSocket> const& socket, std::string path, uint32 userId,
        std::vector<std::string> watch);

    // what ("comments", "reactions", "content") of target changed: the pages watching it are told
    void Changed(std::string target, std::string what);

    void Update(uint32 diff);

    // Shutdown: what the users online now were online for and not written yet, written (blocking)
    void SaveOnlineTime();

    struct OnlineUser
    {
        SystemTimePoint Since;              // online without a break since
        std::set<std::string> Pages;        // the pages open
        std::size_t Connections = 0;
        Seconds Unsaved = 0s;               // not in user.online_seconds yet
    };
    // By user id
    std::map<uint32, OnlineUser> GetOnlineUsers() const;
    Optional<OnlineUser> FindOnlineUser(uint32 userId) const;

    // To every connection
    void Broadcast(std::string_view type, boost::json::object const& body);
    // To connections watching target, or of userId, or of anyone logged in. Watching is open to every
    // page: what goes to watchers must be what anybody may read (a private chat's messages go by user)
    void SendToWatchers(std::string_view target, std::string_view type, boost::json::object const& body);
    void SendToUser(uint32 userId, std::string_view type, boost::json::object const& body);
    void SendToLoggedIn(std::string_view type, boost::json::object const& body);

    std::size_t GetViewerCount() const { return _viewers.size(); }
    // People reading, as the last refresh counted them (the footer's number)
    std::size_t GetReaderCount() const { return _readers; }

private:
    struct Viewer
    {
        std::weak_ptr<Rooftree::Server::WorldWebSocket> Socket;
        std::string Path;
        uint32 UserId = 0;
        std::string Visitor;        // "user:42", or the address's session key
        std::vector<std::string> Watch;
        TimePoint LastSeen;
        // What it was told last, nothing sent yet: -1
        int64 SentPage = -1;
        int64 SentSite = -1;
    };

    struct Online
    {
        SystemTimePoint Since;
        TimePoint SavedUntil;       // online time before this is written
        TimePoint LastActive;       // the last refresh with a page connected
    };

    void Refresh();
    // The changes since the last refresh, to the pages watching them
    void SendChanges();
    // Who is online, from the viewers
    void RefreshOnline(TimePoint now);
    void SaveOnlineTime(uint32 userId, Online& online, TimePoint until, bool blocking);

    // By the connection; the weak pointer tells whether it's still the same one
    std::unordered_map<Rooftree::Server::WorldWebSocket const*, Viewer> _viewers;
    std::unordered_map<uint32, Online> _online;
    // Target and what, not sent yet
    std::set<std::pair<std::string, std::string>> _changes;
    std::size_t _readers = 0;
    Milliseconds _sinceRefresh = 0ms;
    Milliseconds _sinceSave = 0ms;
};
}

#endif // BLOG_PRESENCE_H
