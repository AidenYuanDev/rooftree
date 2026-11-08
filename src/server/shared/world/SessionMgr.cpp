/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "SessionMgr.h"
#include "Config.h"
#include "Log.h"
#include "StringFormat.h"
#include <algorithm>

namespace Rooftree::Server
{
namespace
{
// "14 s", or "250 ms" below a second
std::string FormatDuration(Milliseconds duration)
{
    if (duration < 1s)
        return Rooftree::StringFormat("{} ms", duration.count());
    return Rooftree::StringFormat("{} s", std::chrono::duration_cast<Seconds>(duration).count());
}

// A class's keys at path over base
SessionClassSettings ReadClass(Config const& config, std::string const& path, SessionClassSettings settings)
{
    auto seconds = [&](std::string const& key, Milliseconds value)
    {
        return Milliseconds(Seconds(config.Get<uint32>(path + key, uint32(std::chrono::duration_cast<Seconds>(value).count()))));
    };

    settings.Priority = config.Get<uint32>(path + ".Priority", settings.Priority);
    settings.MaxMessagesPerSessionPerUpdate = std::max(1u, config.Get<uint32>(path + ".MaxMessagesPerSessionPerUpdate", settings.MaxMessagesPerSessionPerUpdate));
    settings.MaxMessagesPerUpdate = config.Get<uint32>(path + ".MaxMessagesPerUpdate", settings.MaxMessagesPerUpdate);
    settings.MaxQueuedMessagesPerSession = std::max(1u, config.Get<uint32>(path + ".MaxQueuedMessagesPerSession", settings.MaxQueuedMessagesPerSession));
    settings.MaxPendingWorkPerSession = config.Get<uint32>(path + ".MaxPendingWorkPerSession", settings.MaxPendingWorkPerSession);
    settings.MaxSessions = config.Get<uint32>(path + ".MaxSessions", settings.MaxSessions);
    settings.SessionIdleTimeout = seconds(".SessionIdleTimeout", settings.SessionIdleTimeout);
    settings.SessionIdleTimeoutWhenBusy = seconds(".SessionIdleTimeoutWhenBusy", settings.SessionIdleTimeoutWhenBusy);

    std::string block = config.Get<std::string>(path + ".Block", settings.Block == BlockScope::Session ? "Session" : "Address");
    if (block == "Session")
        settings.Block = BlockScope::Session;
    else if (block == "Address")
        settings.Block = BlockScope::Address;
    else
        RT_LOG_ERROR("server", "{}.Block must be Address or Session, not {}; kept {}", path, block,
            settings.Block == BlockScope::Session ? "Session" : "Address");
    return settings;
}
}

SessionClassSettings SessionMgrSettings::GetDefaultClass() const
{
    return { .Priority = Priority, .MaxMessagesPerSessionPerUpdate = MaxMessagesPerSessionPerUpdate, .MaxMessagesPerUpdate = MaxMessagesPerUpdate,
        .MaxQueuedMessagesPerSession = MaxQueuedMessagesPerSession, .MaxPendingWorkPerSession = MaxPendingWorkPerSession,
        .MaxSessions = MaxSessions, .SessionIdleTimeout = SessionIdleTimeout, .SessionIdleTimeoutWhenBusy = SessionIdleTimeoutWhenBusy,
        .Block = Block };
}

SessionMgrSettings SessionMgrSettings::FromConfig(Config const& config, std::string const& path)
{
    SessionMgrSettings settings;
    SessionClassSettings defaults = ReadClass(config, path, settings.GetDefaultClass());
    settings.Priority = defaults.Priority;
    settings.MaxMessagesPerSessionPerUpdate = defaults.MaxMessagesPerSessionPerUpdate;
    settings.MaxMessagesPerUpdate = defaults.MaxMessagesPerUpdate;
    settings.MaxQueuedMessagesPerSession = defaults.MaxQueuedMessagesPerSession;
    settings.MaxPendingWorkPerSession = defaults.MaxPendingWorkPerSession;
    settings.MaxSessions = defaults.MaxSessions;
    settings.SessionIdleTimeout = defaults.SessionIdleTimeout;
    settings.SessionIdleTimeoutWhenBusy = defaults.SessionIdleTimeoutWhenBusy;
    settings.Block = defaults.Block;
    settings.MaxPendingMessages = std::max(1u, config.Get<uint32>(path + ".MaxPendingMessages", settings.MaxPendingMessages));

    // Class names are a mapping's keys, walk it
    if (YAML::Node classes = config.GetNode(path + ".Classes"); classes && classes.IsMap())
        for (auto const& entry : classes)
        {
            std::string name = entry.first.as<std::string>();
            settings.Classes[name] = ReadClass(config, path + ".Classes." + name, defaults);
        }
    else if (classes)
        RT_LOG_ERROR("server", "{}.Classes must be a mapping of class name -> its settings", path);

    return settings;
}

SessionMgr::SessionMgr(std::size_t workerThreads) : _workers(std::make_unique<ThreadPool>(std::max<std::size_t>(workerThreads, 1)))
{
    _classes.push_back(std::make_unique<SessionClass>());
    ApplyClassSettings();
}

SessionMgr::~SessionMgr()
{
    Stop();
}

void SessionMgr::LoadSettings(SessionMgrSettings const& settings, AntiDosSettings const& antiDos)
{
    _settings = settings;
    _antiDosSettings = antiDos;
    _antiDosChanged = true;
    ApplyClassSettings();
}

uint32 SessionMgr::GetClassIndex(std::string_view name)
{
    for (uint32 i = 0; i < _classes.size(); ++i)
        if (_classes[i]->Name == name)
            return i;

    RT_LOG_WARN("server", "Session class {} has no settings, it gets the default class's", name);
    std::unique_ptr<SessionClass>& added = _classes.emplace_back(std::make_unique<SessionClass>());
    added->Name = std::string(name);
    ApplyClassSettings();
    return uint32(_classes.size() - 1);
}

void SessionMgr::ApplyClassSettings()
{
    // A class the settings no longer name keeps its sessions, with the default class's settings
    for (std::size_t i = 0; i < _classes.size(); ++i)
    {
        auto itr = _settings.Classes.find(_classes[i]->Name);
        _classes[i]->Settings = i && itr != _settings.Classes.end() ? itr->second : _settings.GetDefaultClass();
    }

    for (auto const& [name, settings] : _settings.Classes)
        if (!name.empty() && std::ranges::none_of(_classes, [&](auto const& sessionClass) { return sessionClass->Name == name; }))
        {
            std::unique_ptr<SessionClass>& added = _classes.emplace_back(std::make_unique<SessionClass>());
            added->Name = name;
            added->Settings = settings;
        }

    _classOrder.clear();
    for (std::unique_ptr<SessionClass> const& sessionClass : _classes)
        _classOrder.push_back(sessionClass.get());
    std::ranges::stable_sort(_classOrder, {}, [](SessionClass const* sessionClass) { return sessionClass->Settings.Priority; });
}

std::size_t SessionMgr::GetSessionCount(std::string_view sessionClass) const
{
    for (std::unique_ptr<SessionClass> const& candidate : _classes)
        if (candidate->Name == sessionClass)
            return candidate->Sessions.size();
    return 0;
}

bool SessionMgr::Enqueue(std::unique_ptr<Message> message)
{
    if (_blocklist->IsBlocked(message->GetRemoteAddress(), TimePoint::clock::now()))
    {
        _blockedDropped.fetch_add(1, std::memory_order_relaxed);
        message->Reject(RejectReason::Blocked);
        return false;
    }

    // Relaxed: the bound only has to hold roughly, a few messages over it are fine
    if (_inboxSize.fetch_add(1, std::memory_order_relaxed) >= _settings.MaxPendingMessages)
    {
        _inboxSize.fetch_sub(1, std::memory_order_relaxed);
        _inboxFull.fetch_add(1, std::memory_order_relaxed);
        message->Reject(RejectReason::Overloaded);
        return false;
    }

    _inbox.Enqueue(message.release());
    return true;
}

void SessionMgr::Update(uint32 diff)
{
    TimePoint now = TimePoint::clock::now();
    ++_stats.Updates;

    // Not in LoadSettings: an app loads its settings before its routes register their types
    if (_antiDosChanged)
    {
        _antiDos.Load(_antiDosSettings, _messageTypes);
        _antiDosChanged = false;
    }

    RunCompletions();
    MoveInboxToSessions(now);

    // The game's sessions before the web's, each class within its own budget
    uint32 processed = 0;
    for (SessionClass* sessionClass : _classOrder)
        processed += UpdateClass(*sessionClass, diff, now);

    _stats.ProcessedMessages += processed;
    _stats.MaxProcessedPerUpdate = std::max(_stats.MaxProcessedPerUpdate, processed);

    for (std::unique_ptr<SessionClass> const& sessionClass : _classes)
        RemoveSessions(*sessionClass, now);
    _blocklist->Prune(now);
    std::erase_if(_blockedKeys, [now](auto const& entry)
    {
        if (entry.second > now)
            return false;

        RT_LOG_INFO("server.antidos", "AntiDOS: {} is no longer refused", entry.first);
        return true;
    });
    LogRejections(now);

    uint32 queued = 0;
    ForEachSession([&queued](Session const& session) { queued += uint32(session._queue.size()); });
    _stats.MaxQueuedMessages = std::max(_stats.MaxQueuedMessages, queued);
}

uint32 SessionMgr::UpdateClass(SessionClass& sessionClass, uint32 diff, TimePoint now)
{
    SessionClassSettings const& settings = sessionClass.Settings;
    std::vector<std::unique_ptr<Session>>& sessions = sessionClass.Sessions;
    uint32 budget = settings.MaxMessagesPerUpdate ? settings.MaxMessagesPerUpdate : std::numeric_limits<uint32>::max();
    uint32 processed = 0;
    std::size_t count = sessions.size();
    std::size_t start = count ? sessionClass.NextSession % count : 0;
    bool budgetExhausted = false;
    // Sessions created by the callbacks below are updated from the next update on
    for (std::size_t i = 0; i < count; ++i)
    {
        std::size_t index = (start + i) % count;
        Session& session = *sessions[index];

        // A callback or the session's own update that throws closes the session, not the server
        try
        {
            session._queryProcessor.ProcessReadyCallbacks();
            session.Update(diff);
        }
        catch (...)
        {
            RT_LOG_ERROR("server", "Session {}: its update threw: {}; closed", session.GetKey(), DescribeException(std::current_exception()));
            session.Close();
            continue;
        }

        if (budgetExhausted || session._closed || session._queue.empty() || !session.CanProcessMessages())
            continue;

        processed += ProcessMessages(session, std::min(settings.MaxMessagesPerSessionPerUpdate, budget - processed), now);
        if (processed >= budget)
        {
            budgetExhausted = true;
            // The next update serves the sessions this one didn't get to first
            sessionClass.NextSession = index + 1;
        }
    }

    return processed;
}

Session* SessionMgr::FindSession(std::string_view key) const
{
    auto itr = _sessionsByKey.find(key);
    return itr != _sessionsByKey.end() ? itr->second : nullptr;
}

void SessionMgr::Stop()
{
    if (_workers)
    {
        // Let started work finish: its completion references a session, which is still there
        _workers->Join();
        _workers = nullptr;
    }

    Completion completion;
    while (_completions.next(completion))
        --completion.Target->_pendingWork;

    Message* raw;
    while (_inbox.Dequeue(raw))
    {
        --_inboxSize;
        std::unique_ptr<Message> message(raw);
        message->Reject(RejectReason::SessionClosed);
        ++_stats.RejectedMessages;
    }

    for (std::unique_ptr<SessionClass> const& sessionClass : _classes)
    {
        for (std::unique_ptr<Session> const& session : sessionClass->Sessions)
            DetachSession(*session);
        sessionClass->Sessions.clear();
        sessionClass->NextSession = 0;
    }
}

void SessionMgr::RunCompletions()
{
    Completion completion;
    while (_completions.next(completion))
    {
        --completion.Target->_pendingWork;
        try
        {
            completion.Callback(*completion.Target);
        }
        catch (...)
        {
            RT_LOG_ERROR("server", "Session {}: the end of its work threw: {}; closed", completion.Target->GetKey(),
                DescribeException(std::current_exception()));
            completion.Target->Close();
        }
    }
}

void SessionMgr::MoveInboxToSessions(TimePoint now)
{
    Message* raw;
    while (_inbox.Dequeue(raw))
    {
        _inboxSize.fetch_sub(1, std::memory_order_relaxed);
        std::unique_ptr<Message> message(raw);

        if (IsKeyBlocked(message->GetSessionKey(), now))
        {
            message->Reject(RejectReason::RateLimited);
            ++_stats.RejectedMessages;
            ++_rejections.Kicked;
            continue;
        }

        Session* session = FindSession(message->GetSessionKey());
        if (!session)
        {
            uint32 classIndex = GetClassIndex(message->GetType().SessionClass);
            SessionClass& sessionClass = *_classes[classIndex];
            if (sessionClass.Settings.MaxSessions && sessionClass.Sessions.size() >= sessionClass.Settings.MaxSessions)
            {
                message->Reject(RejectReason::Overloaded);
                ++_stats.RejectedMessages;
                ++_rejections.Busy;
                continue;
            }

            std::unique_ptr<Session> created = CreateSession(*message);
            if (!created)
            {
                message->Reject(RejectReason::SessionClosed);
                ++_stats.RejectedMessages;
                continue;
            }

            session = created.get();
            session->_classIndex = classIndex;
            _sessionsByKey[session->GetKey()] = session;
            sessionClass.Sessions.push_back(std::move(created));
        }

        if (session->_closed)
        {
            message->Reject(RejectReason::SessionClosed);
            ++_stats.RejectedMessages;
            continue;
        }

        if (session->_queue.size() >= GetClass(*session).Settings.MaxQueuedMessagesPerSession)
        {
            message->Reject(RejectReason::Overloaded);
            ++_stats.RejectedMessages;
            ++_rejections.Busy;
            CountRejection(*session, *message, now);
            continue;
        }

        session->_lastActivity = now;
        session->_queue.push_back(std::move(message));
    }
}

uint32 SessionMgr::ProcessMessages(Session& session, uint32 limit, TimePoint now)
{
    uint32 maxPendingWork = GetClass(session).Settings.MaxPendingWorkPerSession;
    uint32 processed = 0;
    // Asked before every message: one may hold back those after it, and the work it started too
    while (processed < limit && !session._queue.empty() && !session._closed && session.CanProcessMessages()
        && (!maxPendingWork || session.GetPendingWorkCount() < maxPendingWork))
    {
        std::unique_ptr<Message> message = std::move(session._queue.front());
        session._queue.pop_front();
        ++processed;

        if (!_antiDos.IsExempt(message->GetRemoteAddress()) && !session._antiDos.Evaluate(_antiDos, message->GetType(), now))
        {
            ++_stats.AntiDosViolations;
            AntiDosPolicy policy = _antiDos.GetPolicy();
            if (session._antiDosPeriod != _rejectionsPeriod)
            {
                session._antiDosPeriod = _rejectionsPeriod;
                ++_rejections.SessionsOverLimit;
            }

            if (session._antiDos.CountViolation(message->GetType(), now))
            {
                AntiDosLimit const* limit = _antiDos.GetLimit(message->GetType().Id);
                std::string action = policy == AntiDosPolicy::Log ? "let through (Policy Log); more are counted"
                    : policy == AntiDosPolicy::Reject ? "rejecting; more are counted"
                    : Rooftree::StringFormat("kicked for {}", FormatDuration(_antiDos.GetKickDuration()));
                RT_LOG_WARN("server.antidos", "AntiDOS: session {} ({}) is over the limit of {} ({} per {}), {}",
                    session.GetKey(), message->GetRemoteAddress().to_string(), message->GetType().Name,
                    limit->MaxCount, FormatDuration(limit->Window), action);
            }

            session.OnAntiDosViolation(*message, policy);

            if (policy == AntiDosPolicy::Kick)
            {
                _blockedKeys[session.GetKey()] = now + _antiDos.GetKickDuration();
                session.Close();
            }

            if (policy != AntiDosPolicy::Log)
            {
                message->Reject(RejectReason::RateLimited);
                ++_stats.RejectedMessages;
                ++_rejections.OverLimit;
                CountRejection(session, *message, now);
                continue;
            }
        }

        // Only this message goes: an HTTP request whose handler threw is answered 500 when dropped
        try
        {
            message->Process(session);
        }
        catch (...)
        {
            RT_LOG_ERROR("server", "Session {}: {} threw: {}", session.GetKey(), message->GetType().Name,
                DescribeException(std::current_exception()));
        }
    }

    return processed;
}

bool SessionMgr::CountRejection(Session& session, Message const& message, TimePoint now)
{
    uint32 threshold = _antiDos.GetBlockAfterRejections();
    if (!threshold || session._closed || _antiDos.IsExempt(message.GetRemoteAddress()))
        return false;

    if (now - session._rejectionsSince >= _antiDos.GetBlockAfterWindow())
    {
        session._rejectionsSince = now;
        session._rejectionsInWindow = 0;
    }

    if (++session._rejectionsInWindow < threshold)
        return false;

    _blockedKeys[session.GetKey()] = now + _antiDos.GetKickDuration();
    session.Close();
    if (GetClass(session).Settings.Block == BlockScope::Session)
    {
        // A player: only its account, others play from the same address
        RT_LOG_WARN("server.antidos", "AntiDOS: session {} ({}) was refused {} times within {}, it is refused for {}",
            session.GetKey(), message.GetRemoteAddress().to_string(), session._rejectionsInWindow,
            FormatDuration(_antiDos.GetBlockAfterWindow()), FormatDuration(_antiDos.GetKickDuration()));
        return true;
    }

    // Its connections go too: the network threads drop what comes from it from now on
    _blocklist->Block(message.GetRemoteAddress(), now + _antiDos.GetKickDuration());
    ++_rejections.AddressesBlocked;
    RT_LOG_WARN("server.antidos", "AntiDOS: session {} ({}) was refused {} times within {}, its address is blocked for {}",
        session.GetKey(), message.GetRemoteAddress().to_string(), session._rejectionsInWindow,
        FormatDuration(_antiDos.GetBlockAfterWindow()), FormatDuration(_antiDos.GetKickDuration()));
    return true;
}

void SessionMgr::RemoveSessions(SessionClass& sessionClass, TimePoint now)
{
    SessionClassSettings const& settings = sessionClass.Settings;
    std::vector<std::unique_ptr<Session>>& _sessions = sessionClass.Sessions;
    // Many addresses that each came once (a flood from many, or just a busy day) would hold the table
    // full for SessionIdleTimeout, and every new client would be refused meanwhile
    Milliseconds idleTimeout = settings.SessionIdleTimeout;
    if (settings.MaxSessions && _sessions.size() >= settings.MaxSessions - settings.MaxSessions / 4)
        idleTimeout = std::min(idleTimeout, settings.SessionIdleTimeoutWhenBusy);

    std::vector<std::size_t> removed;
    for (std::size_t i = 0; i < _sessions.size(); ++i)
    {
        Session& session = *_sessions[i];
        if (session._antiDos.HasViolations())
            LogViolations(session, now, false);

        if (session.HasPendingWork())
            continue;

        bool idle = session._queue.empty() && now - session._lastActivity >= idleTimeout
            // A session that is still within an AntiDOS window would otherwise start over with a new one
            && session._antiDos.IsEmpty(now, _antiDos) && session.CanBeRemoved();
        if (session._closed || idle)
            removed.push_back(i);
    }

    if (!removed.empty())
    {
        for (std::size_t index : removed)
        {
            LogViolations(*_sessions[index], now, true);
            DetachSession(*_sessions[index]);
        }

        // One pass, not an erase per session: after a burst of new addresses most of them expire together
        std::size_t kept = 0;
        std::size_t nextSession = sessionClass.NextSession;
        auto next = removed.begin();
        for (std::size_t i = 0; i < _sessions.size(); ++i)
        {
            if (next != removed.end() && *next == i)
            {
                ++next;
                if (i < sessionClass.NextSession)
                    --nextSession;
                continue;
            }

            if (kept != i)
                _sessions[kept] = std::move(_sessions[i]);
            ++kept;
        }

        _sessions.resize(kept);
        sessionClass.NextSession = nextSession;
    }
}

void SessionMgr::DetachSession(Session& session)
{
    for (std::unique_ptr<Message>& message : session._queue)
    {
        message->Reject(RejectReason::SessionClosed);
        ++_stats.RejectedMessages;
    }
    session._queue.clear();

    // Here, while they can still use what the derived session has
    session.DestroyCoroutines();
    session.OnRemoved();
    _sessionsByKey.erase(session.GetKey());
}

bool SessionMgr::IsKeyBlocked(std::string const& key, TimePoint now)
{
    auto itr = _blockedKeys.find(key);
    return itr != _blockedKeys.end() && itr->second > now;
}

void SessionMgr::LogViolations(Session& session, TimePoint now, bool sessionRemoved)
{
    std::string_view verb = _antiDos.GetPolicy() == AntiDosPolicy::Log ? "let through" : "rejected";
    for (AntiDosReport const& report : session._antiDos.TakeReports(_antiDos, now, _settings.AntiDosLogInterval, sessionRemoved))
    {
        std::string_view state = report.RunState == AntiDosReport::State::StillOver ? "still over"
            : report.RunState == AntiDosReport::State::Ended ? "under the limit again" : "session removed";
        if (report.Count)
            RT_LOG_WARN("server.antidos", "AntiDOS: session {} {}: {} more {} in {}, {}",
                session.GetKey(), report.Type->Name, report.Count, verb, FormatDuration(report.Over), state);
        else
            RT_LOG_WARN("server.antidos", "AntiDOS: session {} {}: {}", session.GetKey(), report.Type->Name, state);
    }
}

void SessionMgr::LogRejections(TimePoint now)
{
    if (_lastRejectionsLog == TimePoint())
        _lastRejectionsLog = now;
    if (now - _lastRejectionsLog < _settings.AntiDosLogInterval)
        return;

    uint64 busy = _rejections.Busy + _inboxFull.exchange(0, std::memory_order_relaxed);
    uint64 blocked = _blockedDropped.exchange(0, std::memory_order_relaxed);
    uint64 rejected = _rejections.OverLimit + _rejections.Kicked + busy + blocked;
    if (rejected || _rejections.SessionsOverLimit)
    {
        std::string parts;
        for (auto [name, count] : { std::pair<std::string_view, uint64>("over a limit", _rejections.OverLimit),
            { "kicked", _rejections.Kicked }, { "server busy", busy }, { "address blocked", blocked } })
            if (count)
                parts += Rooftree::StringFormat("{}{} {}", parts.empty() ? "" : ", ", name, count);

        // "1/100000 sessions", per class once there are more: "sessions: default 1/100000, game 5/5000 (full)"
        auto count = [](SessionClass const& sessionClass)
        {
            std::size_t sessions = sessionClass.Sessions.size();
            uint32 max = sessionClass.Settings.MaxSessions;
            return max ? Rooftree::StringFormat("{}/{}{}", sessions, max, sessions >= max ? " (full)" : "") : std::to_string(sessions);
        };
        std::string sessions;
        if (_classes.size() == 1)
        {
            sessions = count(*_classes[0]);
            std::size_t full = sessions.find(" (full)");
            sessions = full == std::string::npos ? sessions + " sessions" : sessions.substr(0, full) + " sessions (full)";
        }
        else
        {
            sessions = "sessions:";
            for (std::unique_ptr<SessionClass> const& sessionClass : _classes)
                sessions += Rooftree::StringFormat("{} {} {}", sessions.back() == ':' ? "" : ",",
                    sessionClass->Name.empty() ? "default" : sessionClass->Name, count(*sessionClass));
        }
        std::string addresses = _rejections.AddressesBlocked || _blocklist->GetCount()
            ? Rooftree::StringFormat(", {} addresses blocked ({} now)", _rejections.AddressesBlocked, _blocklist->GetCount()) : "";
        RT_LOG_WARN("server.antidos", "AntiDOS: last {}: {} rejected{}, {} sessions over a limit, {}{}",
            FormatDuration(std::chrono::duration_cast<Milliseconds>(now - _lastRejectionsLog)), rejected,
            parts.empty() ? "" : " (" + parts + ")", _rejections.SessionsOverLimit, sessions, addresses);
    }

    _rejections = {};
    ++_rejectionsPeriod;
    _lastRejectionsLog = now;
}
}
