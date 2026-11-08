/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "BlogWorld.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "Config.h"
#include "Console.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringFormat.h"

namespace Blog
{
BlogWorld* sWorld = nullptr;

namespace
{
// Any thread. A site that can't be read leaves the one being served
void LoadSiteInto(Rooftree::Server::StaticSiteHolder& holder, std::string const& directory)
{
    TimePoint start = TimePoint::clock::now();
    std::string error;
    std::shared_ptr<Rooftree::Server::StaticSite const> site = Rooftree::Server::StaticSite::Load(directory, error);
    if (!site)
    {
        RT_LOG_ERROR("blog", "Blog.Static.Directory: {}, the site being served stays", error);
        return;
    }

    holder.Set(site);
    RT_LOG_INFO("blog", "Site of {} loaded: {} files, {} KiB in {} ms", directory, site->GetFileCount(), site->GetSize() / 1024,
        std::chrono::duration_cast<Milliseconds>(TimePoint::clock::now() - start).count());
}

// A made-up or expired cookie is not looked up again for this long
constexpr Seconds InvalidSessionCacheTime = 1min;
// A search waits this long at most for the index with the latest change
constexpr Milliseconds SearchIndexWait = 2s;
// Comment counts are read again this long after the first change, however many come meanwhile
constexpr Milliseconds CommentCountsDelay = 1s;
// Reads of the site are written as sums this often
constexpr Seconds StatsFlushInterval = 1min;
// Drafts whose time came are published this often (at most this late)
constexpr Seconds ScheduledPostsInterval = 30s;
}

BlogWorld::BlogWorld(std::size_t workerThreads) : SessionMgr(workerThreads)
{
    _scheduler.Schedule(10min, [this](TaskContext context)
    {
        Cleanup();
        context.Repeat();
    });
    _scheduler.Schedule(StatsFlushInterval, [this](TaskContext context)
    {
        _analytics.Flush();
        context.Repeat();
    });
    _scheduler.Schedule(ScheduledPostsInterval, [this](TaskContext context)
    {
        if (!_scheduledPublish.valid())
            _scheduledPublish = std::async(std::launch::async, &PublishDuePostsDirect);
        context.Repeat();
    });
}

BlogWorld::~BlogWorld()
{
    if (_rebuildThread.joinable())
        _rebuildThread.join();
}

void BlogWorld::Shutdown()
{
    if (_rebuildThread.joinable())
    {
        RT_LOG_INFO("blog", "Waiting for the running rebuild to finish");
        _rebuildThread.join();
    }

    if (_staticSiteLoad.valid())
        _staticSiteLoad.wait();

    // Their threads read the database, which stops after us
    if (_searchRetired.valid())
        _searchRetired.wait();
    if (_searchBuild.valid())
        _searchBuild.wait();
    if (_commentCountsLoad.valid())
        _commentCountsLoad.wait();
    if (_scheduledPublish.valid())
        _scheduledPublish.wait();

    _newsletter.Stop();
    _presence.SaveOnlineTime();
    _collab.SaveAll();
    // The reads counted since the last minute, blocking like the online time
    Analytics::Write(_analytics.Take(), true);
    Stop();
}

void BlogWorld::LoadSettings(BlogSettings settings)
{
    _settings = std::move(settings);
    SessionMgr::LoadSettings(_settings.Sessions, _settings.AntiDos);
    _loginFailures.SetLimit(_settings.LoginFailuresPerAccount, _settings.LoginFailureWindow);
    _mailLimiter.SetLimit(1, _settings.MailInterval);
    _captchas.SetLifetime(_settings.CaptchaLifetime);
    _commentLimiter.SetLimit(_settings.CommentsPerUser, _settings.CommentWindow);
    _avatarLimiter.SetLimit(_settings.AvatarChangesPerUser, _settings.AvatarWindow);
    _status.SetDataDirectory(_settings.UploadDirectory);
    _status.SetSampleInterval(_settings.StatusSampleInterval);
    _analytics.SetDayOffset(_settings.StatsDayOffset);
    _worldTicker.SetInterval(_settings.WorldTick);
    _worldTicker.SetMaxCatchUp(_settings.WorldTickMaxCatchUp);
}

void BlogWorld::Update(uint32 diff)
{
    // Between two updates nothing runs, so the settings change all at once
    if (_reloadRequested.exchange(false))
        Reload();

    TimePoint start = TimePoint::clock::now();
    SessionMgr::Update(diff);
    _scheduler.Update(Milliseconds(diff));
    _presence.Update(diff);
    _collab.Update(diff);
    UpdateRebuild();
    // Last: the changes and searches of this update are all in
    UpdateSearch();
    UpdateCommentCounts();
    UpdateScheduledPosts();

    // The game's world, at its own tick: the messages above go on every update meanwhile
    if (!_worldTicks.empty())
    {
        _worldTicker.Update(diff, [this](uint32 tick)
        {
            for (std::function<void(uint32)> const& worldTick : _worldTicks)
            {
                // One that throws doesn't stop the others, nor the next tick
                try
                {
                    worldTick(tick);
                }
                catch (...)
                {
                    RT_LOG_ERROR("server.world", "A world tick threw: {}", Rooftree::Server::DescribeException(std::current_exception()));
                }
            }
        });

        TimePoint now = TimePoint::clock::now();
        if (_worldTickStatsSince == TimePoint())
            _worldTickStatsSince = now;
        else if (now - _worldTickStatsSince >= 1min)
        {
            Rooftree::Server::FixedTickerStats stats = _worldTicker.TakeStats();
            RT_LOG_INFO("server.world", "{} world ticks of {} ms, tick time avg {} us max {} us, {} dropped (behind)",
                stats.Ticks, _worldTicker.GetInterval().count(), stats.Ticks ? stats.TotalTickTime.count() / stats.Ticks : 0,
                stats.MaxTickTime.count(), stats.Dropped);
            _worldTickStatsSince = now;
        }
    }

    _status.Update(diff, std::chrono::duration_cast<std::chrono::microseconds>(TimePoint::clock::now() - start), _presence);
}

void BlogWorld::AnnounceRestart(Optional<uint32> seconds)
{
    _presence.Broadcast("restart", { { "in", seconds ? boost::json::value(*seconds) : boost::json::value(nullptr) } });
}

void BlogWorld::AnnouncePublished(std::string_view kind, std::string path, std::string title)
{
    _presence.Broadcast("published", { { "kind", kind }, { "path", std::move(path) }, { "title", std::move(title) } });
}

void BlogWorld::Reload()
{
    std::string error;
    if (!sConfigMgr->Reload(error))
    {
        RT_LOG_ERROR("blog", "Config reload failed, keeping the current one: {}", error);
        return;
    }

    sLog->LoadFromConfig();
    BlogSettings settings;
    if (!settings.LoadFromConfig(*sConfigMgr->GetConfig()))
    {
        RT_LOG_ERROR("blog", "The reloaded config has invalid Blog settings, keeping the current ones");
        return;
    }

    LoadSettings(std::move(settings));
    if (_onReloaded)
        _onReloaded(*sConfigMgr->GetConfig());
    RT_LOG_INFO("blog", "Config reloaded");
    // After a rebuild by hand, or another directory
    LoadStaticSite();
}

void BlogWorld::Cleanup()
{
    // Its answers come back on the next read; what made up keys (cursors) filled it with doesn't
    _readCache.Invalidate();
    TimePoint now = TimePoint::clock::now();
    std::erase_if(_authCache, [&](auto const& entry)
    {
        return now - entry.second.CachedAt >= _settings.AuthCacheLifetime || entry.second.Info.ExpiresAt <= SystemTimePoint::clock::now();
    });
    std::erase_if(_invalidSessions, [now](auto const& entry) { return entry.second <= now; });
    _loginFailures.Cleanup(now);
    _mailLimiter.Cleanup(now);
    _captchas.Cleanup(now);
    std::erase_if(_oauthFlows, [now](auto const& entry) { return entry.second.Expires <= now; });
    std::erase_if(_oauthTickets, [now](auto const& entry) { return entry.second.Expires <= now; });
    _commentLimiter.Cleanup(now);
    _avatarLimiter.Cleanup(now);
    _replyMailLimiter.Cleanup(now);
    SystemTimePoint wallNow = SystemTimePoint::clock::now();
    std::erase_if(_ipBans, [wallNow](IpBan const& ban) { return ban.ExpiresAt && *ban.ExpiresAt <= wallNow; });

    BlogDatabase.Execute(BlogDatabase.GetPreparedStatement(BLOG_DEL_EXPIRED_SESSIONS));
    BlogDatabase.Execute(BlogDatabase.GetPreparedStatement(BLOG_DEL_EXPIRED_EMAIL_TOKENS));
}

AuthInfo const* BlogWorld::FindAuth(std::string const& sessionHash)
{
    auto itr = _authCache.find(sessionHash);
    if (itr == _authCache.end())
        return nullptr;

    if (TimePoint::clock::now() - itr->second.CachedAt >= _settings.AuthCacheLifetime || itr->second.Info.ExpiresAt <= SystemTimePoint::clock::now())
    {
        _authCache.erase(itr);
        return nullptr;
    }

    return &itr->second.Info;
}

bool BlogWorld::IsKnownInvalidSession(std::string const& sessionHash) const
{
    auto itr = _invalidSessions.find(sessionHash);
    return itr != _invalidSessions.end() && itr->second > TimePoint::clock::now();
}

void BlogWorld::CacheAuth(AuthInfo info)
{
    info.User.PasswordHash.clear();
    std::string hash = info.SessionHash;
    _invalidSessions.erase(hash);
    _authCache[hash] = { .Info = std::move(info), .CachedAt = TimePoint::clock::now() };
}

void BlogWorld::CacheInvalidSession(std::string const& sessionHash)
{
    _invalidSessions[sessionHash] = TimePoint::clock::now() + InvalidSessionCacheTime;
}

void BlogWorld::ForgetAuth(std::string const& sessionHash)
{
    _authCache.erase(sessionHash);
    CacheInvalidSession(sessionHash);
}

void BlogWorld::ForgetUserAuth(uint32 userId)
{
    std::erase_if(_authCache, [userId](auto const& entry) { return entry.second.Info.User.Id == userId; });
    // An author's name is on the posts' pages
    _readCache.Invalidate();
}

std::string BlogWorld::GetSiteName(std::string_view lang) const
{
    if (boost::json::value const* names = _siteProfile.if_contains("name"); names && names->is_object())
        for (std::string_view candidate : { lang, lang == "en" ? std::string_view("zh") : std::string_view("en") })
            if (boost::json::value const* name = names->as_object().if_contains(candidate); name && name->is_string() && !name->as_string().empty())
                return std::string(name->as_string());
    return _settings.SiteName;
}

TagInfo const* BlogWorld::FindTag(std::string_view slug) const
{
    auto itr = _tags.find(slug);
    return itr != _tags.end() ? &itr->second : nullptr;
}

void BlogWorld::SetTags(std::vector<TagInfo> tags)
{
    _tags.clear();
    for (TagInfo& tag : tags)
    {
        std::string slug = tag.Slug;
        _tags.emplace(std::move(slug), std::move(tag));
    }
}

namespace
{
Rooftree::Task<void> Deliver(Rooftree::Server::Session& session, Rooftree::Server::MailSettings settings, std::string outbox,
    Rooftree::Server::MailMessage message)
{
    co_await session.OnWorker([&] { DeliverMail(settings, outbox, message); });
}
}

void BlogWorld::SendMail(Rooftree::Server::Session& session, Rooftree::Server::MailMessage message)
{
    session.Spawn(Deliver(session, _settings.Mail, _settings.MailOutboxDirectory, std::move(message)));
}

void BlogWorld::QueueNewsletter(std::string_view kind, uint32 id)
{
    _newsletter.Queue({ .Kind = std::string(kind), .Id = id, .PublicOrigin = _settings.PublicOrigin,
        .SiteNames = { { "zh", GetSiteName("zh") }, { "en", GetSiteName("en") } }, .Mail = _settings.Mail,
        .Outbox = _settings.MailOutboxDirectory, .MailsPerMinute = _settings.NewsletterMailsPerMinute });
}

std::string BlogWorld::StartOAuthFlow(OAuthFlow flow)
{
    // Each one is kept 10 minutes: AntiDOS (auth.oauth) keeps one address from filling this
    constexpr std::size_t MaxFlows = 10000;
    if (_oauthFlows.size() >= MaxFlows)
    {
        TimePoint now = TimePoint::clock::now();
        std::erase_if(_oauthFlows, [now](auto const& entry) { return entry.second.Expires <= now; });
        if (_oauthFlows.size() >= MaxFlows)
            return {};
    }

    std::string state = Rooftree::Server::GenerateToken();
    _oauthFlows[Rooftree::Server::Sha256Hex(state)] = std::move(flow);
    return state;
}

Optional<BlogWorld::OAuthFlow> BlogWorld::TakeOAuthFlow(std::string_view state)
{
    auto itr = _oauthFlows.find(Rooftree::Server::Sha256Hex(state));
    if (itr == _oauthFlows.end())
        return {};

    OAuthFlow flow = std::move(itr->second);
    _oauthFlows.erase(itr);
    if (flow.Expires <= TimePoint::clock::now())
        return {};
    return flow;
}

std::string BlogWorld::AddOAuthTicket(OAuthTicket ticket)
{
    std::string token = Rooftree::Server::GenerateToken();
    _oauthTickets[Rooftree::Server::Sha256Hex(token)] = std::move(ticket);
    return token;
}

BlogWorld::OAuthTicket* BlogWorld::FindOAuthTicket(std::string_view ticket)
{
    auto itr = _oauthTickets.find(Rooftree::Server::Sha256Hex(ticket));
    if (itr == _oauthTickets.end() || itr->second.Expires <= TimePoint::clock::now())
        return nullptr;
    return &itr->second;
}

void BlogWorld::RemoveOAuthTicket(std::string_view ticket)
{
    _oauthTickets.erase(Rooftree::Server::Sha256Hex(ticket));
}

bool BlogWorld::IsIpBanned(boost::asio::ip::address const& address) const
{
    std::string ip = address.to_string();
    SystemTimePoint now = SystemTimePoint::clock::now();
    return std::ranges::any_of(_ipBans, [&](IpBan const& ban) { return ban.Ip == ip && (!ban.ExpiresAt || *ban.ExpiresAt > now); });
}

void BlogWorld::RequestRebuild()
{
    // The search has it too, with or without a site to rebuild, and the readers' answers
    RequestSearchIndex();
    _readCache.Invalidate();

    if (_settings.RebuildCommand.empty())
    {
        RT_LOG_DEBUG("blog.rebuild", "Published content changed, Blog.Rebuild.Command is not set");
        return;
    }

    _rebuildDueAt = TimePoint::clock::now() + _settings.RebuildDebounce;
}

void BlogWorld::UpdateRebuild()
{
    if (!_rebuildDueAt || *_rebuildDueAt > TimePoint::clock::now() || _rebuildRunning)
        return;

    if (_rebuildThread.joinable())
        _rebuildThread.join();

    _rebuildDueAt.reset();
    StartRebuild();
}

void BlogWorld::LoadStaticSite(bool wait)
{
    if (_settings.StaticDirectory.empty())
        return;

    if (_staticSiteLoad.valid())
        _staticSiteLoad.wait();
    _staticSiteLoad = std::async(std::launch::async, [this, directory = _settings.StaticDirectory]
    {
        LoadSiteInto(_staticSite, directory);
    });
    if (wait)
        _staticSiteLoad.wait();
}

void BlogWorld::StartRebuild()
{
    std::string command = _settings.RebuildCommand;
    _rebuildRunning = true;
    RT_LOG_INFO("blog.rebuild", "Rebuilding the site: {}", command);

    // Its own thread, not a worker: a build takes a while and would hold a worker for all of it
    _rebuildThread = std::thread([this, command, staticDirectory = _settings.StaticDirectory]
    {
        TimePoint start = TimePoint::clock::now();
        std::string output;
        int status = -1;
        if (FILE* pipe = popen((command + " 2>&1").c_str(), "r"))
        {
            char buffer[4096];
            while (std::size_t read = std::fread(buffer, 1, sizeof(buffer), pipe))
            {
                output.append(buffer, read);
                // Only the end is of interest when it fails
                if (output.size() > 64 * 1024)
                    output.erase(0, output.size() - 32 * 1024);
            }
            status = pclose(pipe);
        }

        long seconds = long(std::chrono::duration_cast<Seconds>(TimePoint::clock::now() - start).count());
        if (status == 0)
        {
            RT_LOG_INFO("blog.rebuild", "Rebuilt the site in {} s", seconds);
            // The command put the new release live, it is served from memory
            if (!staticDirectory.empty())
                LoadSiteInto(_staticSite, staticDirectory);
        }
        else
            RT_LOG_ERROR("blog.rebuild", "Rebuilding the site failed after {} s (status {}), the old pages stay:\n{}", seconds, status,
                output.size() > 4000 ? output.substr(output.size() - 4000) : output);

        _rebuildRunning = false;
    });
}

std::unique_ptr<Rooftree::Server::Session> BlogWorld::CreateSession(Rooftree::Server::Message const& firstMessage)
{
    if (Rooftree::Server::IsConsoleMessage(firstMessage))
        return std::make_unique<Rooftree::Server::ConsoleSession>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    return std::make_unique<BlogSession>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
}
void BlogWorld::WithSearchIndex(std::function<void(SearchIndex const&)> search)
{
    if (_searchBuilt == _searchWanted)
        return search(*_searchIndex);
    _waitingSearches.push_back({ std::move(search), TimePoint::clock::now() });
}

void BlogWorld::UpdateSearch()
{
    // The index being built is done: in, and the one before freed on a thread of its own (a few
    // million small vectors, too long for an update)
    if (_searchBuilding && _searchBuild.wait_for(0s) == std::future_status::ready)
    {
        std::unique_ptr<SearchIndex> retired = std::exchange(_searchIndex, _searchBuild.get());
        _searchBuilt = *_searchBuilding;
        _searchBuilding.reset();
        RT_LOG_DEBUG("blog", "Search index of {} documents built in {} ms", _searchIndex->GetDocumentCount(),
            std::chrono::duration_cast<Milliseconds>(TimePoint::clock::now() - _searchBuildStart).count());
        if (_searchRetired.valid())
            _searchRetired.wait();
        _searchRetired = std::async(std::launch::async, [retired = std::move(retired)]() mutable { retired.reset(); });
    }

    // Changes the index doesn't have: built again. Changes during a build make another one after it
    if (!_searchBuilding && _searchBuilt != _searchWanted)
    {
        _searchBuilding = _searchWanted;
        _searchBuildStart = TimePoint::clock::now();
        _searchBuild = std::async(std::launch::async, &SearchIndex::Load);
    }

    if (_waitingSearches.empty())
        return;
    bool current = _searchBuilt == _searchWanted;
    if (!current && TimePoint::clock::now() - _waitingSearches.front().Since < SearchIndexWait)
        return;
    if (!current)
        RT_LOG_WARN("blog", "The search index is late, {} searches get the one before", _waitingSearches.size());
    for (WaitingSearch& waiting : std::exchange(_waitingSearches, {}))
        waiting.Search(*_searchIndex);
}

void BlogWorld::WakeForScheduledPost(SystemTimePoint at)
{
    // A second after: the database's DATETIME may have rounded the time up
    at += 1s;
    if (!_nextScheduledPost || at < *_nextScheduledPost)
        _nextScheduledPost = at;
}

void BlogWorld::UpdateScheduledPosts()
{
    if (_nextScheduledPost && *_nextScheduledPost <= SystemTimePoint::clock::now() && !_scheduledPublish.valid())
    {
        _nextScheduledPost.reset();
        _scheduledPublish = std::async(std::launch::async, &PublishDuePostsDirect);
    }
    if (!_scheduledPublish.valid() || _scheduledPublish.wait_for(0s) != std::future_status::ready)
        return;

    // As when the admin publishes it: the site again, the open pages told, the subscribers mailed
    for (ScheduledPost const& post : _scheduledPublish.get())
    {
        RT_LOG_INFO("blog.admin", "Published post {} ({}) at its time", post.Slug, post.Id);
        RequestRebuild();
        AnnounceChanged(Rooftree::StringFormat("post:{}", post.Id), "content");
        if (post.PassagesChanged)
            AnnounceChanged(Rooftree::StringFormat("post:{}", post.Id), "comments");
        AnnouncePublished("post", "/posts/" + post.Slug, post.Title);
        QueueNewsletter("post", post.Id);
    }
}

void BlogWorld::RequestCommentCounts()
{
    // The posts show how many they have
    _readCache.Invalidate();
    if (!_commentCountsDueAt)
        _commentCountsDueAt = TimePoint::clock::now() + CommentCountsDelay;
}

int64 BlogWorld::GetCommentCount(uint32 postId) const
{
    auto it = _commentCounts.find(postId);
    return it == _commentCounts.end() ? 0 : it->second;
}

void BlogWorld::UpdateCommentCounts()
{
    if (_commentCountsLoad.valid() && _commentCountsLoad.wait_for(0s) == std::future_status::ready)
        _commentCounts = _commentCountsLoad.get();

    // A change during a load: read again after it
    if (_commentCountsDueAt && *_commentCountsDueAt <= TimePoint::clock::now() && !_commentCountsLoad.valid())
    {
        _commentCountsDueAt.reset();
        _commentCountsLoad = std::async(std::launch::async, &LoadCommentCountsDirect);
    }
}

std::unordered_map<uint32, int64> LoadCommentCountsDirect()
{
    std::unordered_map<uint32, int64> counts;
    if (PreparedQueryResult result = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_COMMENT_COUNTS)))
    {
        do
        {
            Field* fields = result->Fetch();
            counts[fields[0].GetUInt32()] = fields[1].GetInt64();
        } while (result->NextRow());
    }
    return counts;
}
}
