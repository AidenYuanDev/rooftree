/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The blog's World: rooftree-server's SessionMgr plus what all sessions share, all on the main thread:
 *
 *  - the auth cache: session cookie hash -> logged in user. A request with a cookie the cache knows
 *    needs no database; an unknown one is looked up once, a cookie that doesn't exist is remembered
 *    for a minute so made-up cookies don't each cost a query
 *  - rate limiters keyed by something else than the client address (AntiDOS limits per address):
 *    login failures per login name, mails per address
 *  - the tags, few and read by every post list
 *  - the search index (SearchIndex.h) and the posts' comment counts its results show
 *  - periodic cleanup of expired rows and of the limiters
 *  - the status page's samples (ServerStatus.h)
 *
 * The HTTP sessions themselves are per client address (see WorldHttpService.h), a BlogSession runs
 * the requests of everyone behind one address and finds out per request who is logged in.
 */

#ifndef BLOG_WORLD_H
#define BLOG_WORLD_H

#include "Analytics.h"
#include "BlogSettings.h"
#include "FixedTicker.h"
#include "BlogTypes.h"
#include "Captcha.h"
#include "Mailer.h"
#include "Newsletter.h"
#include "Collab.h"
#include "Presence.h"
#include "ReadCache.h"
#include "RateLimiter.h"
#include "SearchIndex.h"
#include "ServerStatus.h"
#include "SessionMgr.h"
#include "StaticSite.h"
#include "TaskScheduler.h"
#include <atomic>
#include <functional>
#include <future>
#include <map>
#include <thread>
#include <unordered_map>

namespace Blog
{
struct TagInfo
{
    uint32 Id = 0;
    std::string Slug;
    std::string NameZh;
    std::string NameEn;
    uint32 PostCount = 0;           // published, in any language
    uint32 PostCountZh = 0;         // published with a Chinese version, English
    uint32 PostCountEn = 0;

    std::string const& GetName(std::string_view lang) const { return lang == "en" ? NameEn : NameZh; }
    uint32 GetPostCount(std::string_view lang) const { return lang.empty() ? PostCount : lang == "en" ? PostCountEn : PostCountZh; }
};

struct IpBan
{
    std::string Ip;
    std::string Reason;
    SystemTimePoint CreatedAt;
    Optional<SystemTimePoint> ExpiresAt;    // none: for good
};

// A draft the server published at its time
struct ScheduledPost
{
    uint32 Id = 0;
    std::string Slug;
    std::string Title;      // in its original language
    bool PassagesChanged = false;
};

class BlogWorld final : public Rooftree::Server::SessionMgr
{
public:
    explicit BlogWorld(std::size_t workerThreads);
    ~BlogWorld();

    // Shutdown, after the network stopped: waits for a running rebuild, then SessionMgr::Stop
    void Shutdown();

    // Main thread, at startup and on every reload
    void LoadSettings(BlogSettings settings);
    BlogSettings const& GetSettings() const { return _settings; }

    void Update(uint32 diff);

    // Main thread: called every Server.WorldTick (50 ms) with the tick, after the update's messages: the
    // game's world (FixedTicker). The loop goes on answering messages every MinUpdateDiff meanwhile
    void AddWorldTick(std::function<void(uint32 tick)> tick) { _worldTicks.push_back(std::move(tick)); }

    // Any thread: reload the config at the next update
    void RequestReload() { _reloadRequested = true; }
    // Main thread: after a reload that worked, for what lives outside the world (the main loop's settings)
    void SetOnReloaded(std::function<void(Config const&)> onReloaded) { _onReloaded = std::move(onReloaded); }

    // Main thread: every page is told the server stops in seconds, or
    // that it doesn't any more (nullopt)
    void AnnounceRestart(Optional<uint32> seconds);

    // Auth cache
    AuthInfo const* FindAuth(std::string const& sessionHash);
    bool IsKnownInvalidSession(std::string const& sessionHash) const;
    void CacheAuth(AuthInfo info);
    void CacheInvalidSession(std::string const& sessionHash);
    void ForgetAuth(std::string const& sessionHash);
    void ForgetUserAuth(uint32 userId);                 // every session of the user (password change, ban)
    template<typename Callable>
    void UpdateCachedUser(uint32 userId, Callable&& update)
    {
        for (auto& [hash, entry] : _authCache)
            if (entry.Info.User.Id == userId)
                update(entry.Info.User);
        // An author's name is on the posts' pages
        _readCache.Invalidate();
    }

    Rooftree::Server::RateLimiter<std::string>& GetLoginFailures() { return _loginFailures; }
    Rooftree::Server::RateLimiter<std::string>& GetMailLimiter() { return _mailLimiter; }
    Rooftree::Server::CaptchaStore& GetCaptchas() { return _captchas; }

    // A GitHub or Google login between leaving for the provider and its callback, by its state
    struct OAuthFlow
    {
        std::string Provider;
        uint32 LinkUserId = 0;      // linking to this account rather than logging in
        std::string Lang;
        std::string Next;           // the page to go back to
        std::string CodeVerifier;
        TimePoint Expires;
    };
    // The state for the provider and the cookie; empty when too many flows are open
    std::string StartOAuthFlow(OAuthFlow flow);
    // The flow of state, once
    Optional<OAuthFlow> TakeOAuthFlow(std::string_view state);

    // A login by GitHub or Google into an account with two-step login, waiting for the code
    struct OAuthTicket
    {
        uint32 UserId = 0;
        std::string Next;
        TimePoint Expires;
        uint32 Attempts = 0;
    };
    std::string AddOAuthTicket(OAuthTicket ticket);
    // Null when unknown or expired
    OAuthTicket* FindOAuthTicket(std::string_view ticket);
    void RemoveOAuthTicket(std::string_view ticket);
    Rooftree::Server::RateLimiter<uint32>& GetCommentLimiter() { return _commentLimiter; }
    Rooftree::Server::RateLimiter<uint32>& GetAvatarLimiter() { return _avatarLimiter; }
    // Reply mails to one account, so a lively thread doesn't flood its mailbox
    Rooftree::Server::RateLimiter<uint32>& GetReplyMailLimiter() { return _replyMailLimiter; }

    // IP bans, from the ip_ban table
    bool IsIpBanned(boost::asio::ip::address const& address) const;
    std::vector<IpBan> const& GetIpBans() const { return _ipBans; }
    void SetIpBans(std::vector<IpBan> bans) { _ipBans = std::move(bans); }

    // Readers' answers kept until what they show changes (ReadCache.h)
    ReadCache& GetReadCache() { return _readCache; }

    // Tags, by slug
    std::map<std::string, TagInfo, std::less<>> const& GetTags() const { return _tags; }
    TagInfo const* FindTag(std::string_view slug) const;
    void SetTags(std::vector<TagInfo> tags);

    // Who reads what, over the WebSockets
    Presence& GetPresence() { return _presence; }
    Collab& GetCollab() { return _collab; }

    // The status page's numbers
    ServerStatus const& GetServerStatus() const { return _status; }
    ServerStatus& GetServerStatus() { return _status; }

    // Reads of the site's pages
    Analytics& GetAnalytics() { return _analytics; }

    // Something was published: the open pages are told (a notice with a link)
    void AnnouncePublished(std::string_view kind, std::string path, std::string title);
    // what ("comments", "reactions", "content") of target ("post:12", "guestbook", "*": all) changed:
    // the open pages showing it ask again (Presence.h)
    void AnnounceChanged(std::string target, std::string what) { _presence.Changed(std::move(target), std::move(what)); }

    // The site's profile (SiteHandlers.cpp), as the admin saved it
    boost::json::object const& GetSiteProfile() const { return _siteProfile; }
    void SetSiteProfile(boost::json::object profile) { _siteProfile = std::move(profile); }
    // The profile's name in lang (or the other language), else Blog.SiteName
    std::string GetSiteName(std::string_view lang) const;

    // A post (first publication) or note (when asked) to mail to the subscribers, on the newsletter's
    // thread. Nothing happens if it was mailed already
    void QueueNewsletter(std::string_view kind, uint32 id);

    // Main thread: sends (or writes to the outbox) on a worker of session, never blocking
    void SendMail(Rooftree::Server::Session& session, Rooftree::Server::MailMessage message);

    // A published post changed, the prerendered pages are stale: runs Blog.Rebuild.Command once no
    // change came for Blog.Rebuild.Debounce. A change during a rebuild runs it again afterwards
    void RequestRebuild();
    bool IsRebuildRunning() const { return _rebuildRunning; }

    // The prerendered site of Blog.Static.Directory, read by the network threads. Loaded at startup,
    // after every rebuild that succeeded and on a reload (a rebuild by hand), each time on another
    // thread than the main one, and swapped in as a whole. wait: until it is (startup)
    Rooftree::Server::StaticSiteHolder const& GetStaticSite() const { return _staticSite; }
    void LoadStaticSite(bool wait = false);

    // Search. The index is built at startup, then again on its own thread after every change of what
    // is published (RequestRebuild asks for it) and swapped in between two updates. A search that comes
    // while a change isn't in yet waits for the index that has it: what was published is found, what
    // was deleted isn't, as soon as the change answered. At most SearchIndexWait, then the index there is
    void SetSearchIndex(std::unique_ptr<SearchIndex> index) { _searchIndex = std::move(index); }
    void RequestSearchIndex() { ++_searchWanted; }
    void WithSearchIndex(std::function<void(SearchIndex const&)> search);

    // Visible comments per post, for the search's results: one query, at most once a second after a
    // comment appeared, went or changed visibility
    void SetCommentCounts(std::unordered_map<uint32, int64> counts) { _commentCounts = std::move(counts); }
    void RequestCommentCounts();

    // A draft is to be published at that time: checked then (else at most ScheduledPostsInterval late)
    void WakeForScheduledPost(SystemTimePoint at);
    int64 GetCommentCount(uint32 postId) const;

protected:
    std::unique_ptr<Rooftree::Server::Session> CreateSession(Rooftree::Server::Message const& firstMessage) override;

private:
    void Reload();
    void Cleanup();
    void UpdateRebuild();
    void StartRebuild();
    void UpdateSearch();
    void UpdateCommentCounts();
    void UpdateScheduledPosts();

    struct CachedAuth
    {
        AuthInfo Info;
        TimePoint CachedAt;
    };

    BlogSettings _settings;
    std::atomic<bool> _reloadRequested = false;

    std::unordered_map<std::string, CachedAuth> _authCache;
    std::unordered_map<std::string, TimePoint> _invalidSessions;   // until

    Rooftree::Server::RateLimiter<std::string> _loginFailures{ 10, 15min };
    Rooftree::Server::RateLimiter<std::string> _mailLimiter{ 1, 60s };
    Rooftree::Server::CaptchaStore _captchas;
    std::unordered_map<std::string, OAuthFlow> _oauthFlows;         // by the state's hash
    std::unordered_map<std::string, OAuthTicket> _oauthTickets;     // by the ticket's hash
    Rooftree::Server::RateLimiter<uint32> _commentLimiter{ 3, 1min };
    Rooftree::Server::RateLimiter<uint32> _avatarLimiter{ 10, 1h };
    Rooftree::Server::RateLimiter<uint32> _replyMailLimiter{ 10, 1h };
    std::vector<IpBan> _ipBans;

    Rooftree::Server::StaticSiteHolder _staticSite;
    std::future<void> _staticSiteLoad;
    Optional<TimePoint> _rebuildDueAt;
    std::atomic<bool> _rebuildRunning = false;
    std::thread _rebuildThread;

    struct WaitingSearch
    {
        std::function<void(SearchIndex const&)> Search;
        TimePoint Since;
    };

    std::unique_ptr<SearchIndex> _searchIndex;
    uint64 _searchWanted = 0;                   // changes asked for
    uint64 _searchBuilt = 0;                    // changes the index has
    Optional<uint64> _searchBuilding;           // changes the index being built has
    TimePoint _searchBuildStart;
    std::future<std::unique_ptr<SearchIndex>> _searchBuild;
    std::future<void> _searchRetired;           // the index before, freed off the main thread
    std::vector<WaitingSearch> _waitingSearches;

    std::unordered_map<uint32, int64> _commentCounts;
    Optional<TimePoint> _commentCountsDueAt;
    std::future<std::unordered_map<uint32, int64>> _commentCountsLoad;
    std::future<std::vector<ScheduledPost>> _scheduledPublish;     // drafts published at their time
    Optional<SystemTimePoint> _nextScheduledPost;

    std::map<std::string, TagInfo, std::less<>> _tags;
    boost::json::object _siteProfile;
    Presence _presence;
    Collab _collab;
    ServerStatus _status;
    Analytics _analytics;
    Newsletter _newsletter;

    TaskScheduler _scheduler;
    ReadCache _readCache;

    std::function<void(Config const&)> _onReloaded;

    Rooftree::Server::FixedTicker _worldTicker;
    std::vector<std::function<void(uint32)>> _worldTicks;
    TimePoint _worldTickStatsSince;
};

// Visible comments per post, with a blocking query: startup, or the counts' own thread
std::unordered_map<uint32, int64> LoadCommentCountsDirect();

// Publishes the drafts whose time came (PostHandlers.cpp), blocking: on a thread of its own. Those it published
std::vector<ScheduledPost> PublishDuePostsDirect();

// Set up by main, lives until shutdown
extern BlogWorld* sWorld;
}

#endif // BLOG_WORLD_H
