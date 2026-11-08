/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The sessions plus the queue in front of them. Network threads Enqueue() messages into
 * one lock-free inbox; every Update() on the main thread:
 *
 *  1. goes on with the finished OnWorker() work
 *  2. moves the inbox into the sessions' queues, creating a session for a new key (CreateSession)
 *  3. updates every session, class by class (below): its database callbacks, its Update(), then at most
 *     MaxMessagesPerSessionPerUpdate of its messages, each through AntiDOS first. Not past
 *     MaxPendingWorkPerSession of its work under way (OnWorker, query callbacks): one session's
 *     queue of slow requests can't take every worker while the others wait behind it.
 *     MaxMessagesPerUpdate caps the
 *     class's sessions together; the next update then starts with the session after the last one
 *     served, so a busy server still gets to every session in turn
 *  4. removes closed sessions and idle ones, in one pass however many go at once. Idle is
 *     SessionIdleTimeout, SessionIdleTimeoutWhenBusy while three quarters of MaxSessions are taken:
 *     clients that came once and left make room for the next ones instead of filling the table
 *
 * Queues are bounded twice: the inbox (MaxPendingMessages) and each session (MaxQueuedMessagesPerSession).
 * A message that doesn't fit is rejected with Overloaded right away instead of piling up. So is the
 * first message of a new key while there are MaxSessions already: every update walks all sessions.
 *
 * Sessions come in classes, one process running the game and the web pages in one loop.
 * A session's class is its first message's type's (MessageType::SessionClass); "" is the default
 * class, SessionMgrSettings' own fields. Each class has its own SessionClassSettings: its budget per
 * update, per session, queue, work under way, sessions, idle timeouts and what AntiDOS blocks. The
 * classes are served by Priority, lowest first, each with its own budget: a flood of web requests
 * runs out the web's budget, never the game's.
 *
 * An address AntiDOS refused too often is blocked (AntiDosSettings::BlockAfter, GetAddressBlocklist):
 * Enqueue drops its messages at once, and a SocketMgr given the list refuses its connections.
 *
 * Rejections are logged as a summary every AntiDosLogInterval, not one by one: they come as fast as a
 * client sends, and so would the lines. AntiDOS violations, see AntiDos.h.
 */

#ifndef ROOFTREE_SESSION_MGR_H
#define ROOFTREE_SESSION_MGR_H

#include "AddressBlocklist.h"
#include "AntiDos.h"
#include "LockedQueue.h"
#include "MPSCQueue.h"
#include "ServerMessage.h"
#include "ServerSession.h"
#include "ThreadPool.h"
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

class Config;

namespace Rooftree::Server
{
// What AntiDOS blocks once a session was refused too often (AntiDosSettings::BlockAfter)
enum class BlockScope
{
    Address,    // the client's address, its connections dropped: anonymous clients (web pages)
    Session     // the session's key only: logged in players, many of them behind one address
};

struct SessionClassSettings
{
    uint32 Priority = 100;                      // classes are served lowest first
    uint32 MaxMessagesPerSessionPerUpdate = 100;
    uint32 MaxMessagesPerUpdate = 0;            // the class's sessions together, 0: no limit
    uint32 MaxQueuedMessagesPerSession = 64;
    uint32 MaxPendingWorkPerSession = 4;        // 0: no limit
    uint32 MaxSessions = 100000;                // 0: no limit
    Milliseconds SessionIdleTimeout = 5min;
    Milliseconds SessionIdleTimeoutWhenBusy = 10s;  // from 3/4 of MaxSessions on
    BlockScope Block = BlockScope::Address;
};

struct SessionMgrSettings
{
    // The default class ("")
    uint32 Priority = 100;
    uint32 MaxMessagesPerSessionPerUpdate = 100;
    uint32 MaxMessagesPerUpdate = 0;
    uint32 MaxQueuedMessagesPerSession = 64;
    uint32 MaxPendingWorkPerSession = 4;
    uint32 MaxSessions = 100000;
    Milliseconds SessionIdleTimeout = 5min;
    Milliseconds SessionIdleTimeoutWhenBusy = 10s;
    BlockScope Block = BlockScope::Address;

    uint32 MaxPendingMessages = 100000;         // in the inbox, not yet moved to a session
    Milliseconds AntiDosLogInterval = 1min;     // not in the config: runs of violations, the summary

    // The other classes by name
    std::map<std::string, SessionClassSettings, std::less<>> Classes;

    SessionClassSettings GetDefaultClass() const;

    // <path>.MaxMessagesPerSessionPerUpdate, ... (SessionIdleTimeout in seconds, Block Address or
    // Session) for the default class, and <path>.Classes.<name> with the same keys for the others, over
    // the default class's values. Missing keys keep the defaults
    static SessionMgrSettings FromConfig(Config const& config, std::string const& path);
};

struct SessionMgrStats
{
    uint64 Updates = 0;
    uint64 ProcessedMessages = 0;
    uint64 RejectedMessages = 0;        // AntiDOS, full queues, closed sessions
    uint64 AntiDosViolations = 0;
    uint32 MaxQueuedMessages = 0;       // most messages waiting in sessions after an update
    uint32 MaxProcessedPerUpdate = 0;
};

class SessionMgr
{
public:
    explicit SessionMgr(std::size_t workerThreads = 2);
    SessionMgr(SessionMgr const&) = delete;
    SessionMgr& operator=(SessionMgr const&) = delete;
    virtual ~SessionMgr();

    // Before the network starts
    MessageTypeRegistry& GetMessageTypes() { return _messageTypes; }

    // Main thread, again on every config reload. The AntiDOS limits are matched to the registered types
    // at the next Update(), so the types may be registered after it too
    void LoadSettings(SessionMgrSettings const& settings, AntiDosSettings const& antiDos);
    SessionMgrSettings const& GetSettings() const { return _settings; }

    // Any thread. False if the message was rejected: the inbox is full (Overloaded), or its address is
    // blocked (Blocked)
    bool Enqueue(std::unique_ptr<Message> message);

    // For the SocketMgrs in front of it (SetAddressBlocklist), and anything else that blocks addresses
    std::shared_ptr<Net::AddressBlocklist> const& GetAddressBlocklist() const { return _blocklist; }

    // Main thread
    void Update(uint32 diff);

    Session* FindSession(std::string_view key) const;
    std::size_t GetSessionCount() const { return _sessionsByKey.size(); }
    // A class's, "" the default one's
    std::size_t GetSessionCount(std::string_view sessionClass) const;

    template<typename Callable>
    void ForEachSession(Callable&& callable) const
    {
        for (std::unique_ptr<SessionClass> const& sessionClass : _classes)
            for (std::unique_ptr<Session> const& session : sessionClass->Sessions)
                callable(*session);
    }

    SessionMgrStats const& GetStats() const { return _stats; }
    void ResetStats() { _stats = {}; }

    // Shutdown, after the network threads stopped: waits for the workers, rejects every queued
    // message and removes all sessions
    void Stop();

protected:
    // Main thread: the session for a key's first message
    virtual std::unique_ptr<Session> CreateSession(Message const& firstMessage) = 0;

private:
    friend class Session;

    struct Completion
    {
        Session* Target;
        std::function<void(Session&)> Callback;
    };

    struct SessionClass
    {
        std::string Name;
        SessionClassSettings Settings;
        std::vector<std::unique_ptr<Session>> Sessions;
        std::size_t NextSession = 0;
    };

    // The class of that name, made with the default class's settings if none is configured
    uint32 GetClassIndex(std::string_view name);
    void ApplyClassSettings();
    SessionClass& GetClass(Session const& session) const { return *_classes[session._classIndex]; }

    void RunCompletions();
    void MoveInboxToSessions(TimePoint now);
    uint32 UpdateClass(SessionClass& sessionClass, uint32 diff, TimePoint now);
    uint32 ProcessMessages(Session& session, uint32 limit, TimePoint now);
    void RemoveSessions(SessionClass& sessionClass, TimePoint now);
    // Rejects its queued messages and forgets its key; the caller takes it out of its class
    void DetachSession(Session& session);
    bool IsKeyBlocked(std::string const& key, TimePoint now);
    // A rejection of this session's; true if it was one too many and its address is blocked now
    bool CountRejection(Session& session, Message const& message, TimePoint now);
    void LogViolations(Session& session, TimePoint now, bool sessionRemoved);
    void LogRejections(TimePoint now);

    MessageTypeRegistry _messageTypes;
    SessionMgrSettings _settings;
    AntiDosSettings _antiDosSettings;
    bool _antiDosChanged = false;
    AntiDosTable _antiDos;

    MPSCQueue<Message, &Message::QueueLink> _inbox;
    std::atomic<uint32> _inboxSize = 0;

    std::vector<std::unique_ptr<SessionClass>> _classes;              // [0] is the default class; never removed
    std::vector<SessionClass*> _classOrder;                            // by Priority
    std::unordered_map<std::string_view, Session*> _sessionsByKey;     // views into Session::_key
    std::unordered_map<std::string, TimePoint> _blockedKeys;           // kicked by AntiDOS, until
    std::shared_ptr<Net::AddressBlocklist> _blocklist = std::make_shared<Net::AddressBlocklist>();

    std::unique_ptr<ThreadPool> _workers;
    LockedQueue<Completion> _completions;

    SessionMgrStats _stats;

    // Since the last summary
    struct Rejections
    {
        uint64 OverLimit = 0;
        uint64 Kicked = 0;
        uint64 Busy = 0;
        uint32 SessionsOverLimit = 0;
        uint32 AddressesBlocked = 0;
    };
    Rejections _rejections;
    std::atomic<uint64> _inboxFull = 0;         // network threads
    std::atomic<uint64> _blockedDropped = 0;    // network threads: messages of blocked addresses
    uint32 _rejectionsPeriod = 1;               // a session counts once per summary, Session::_antiDosPeriod
    TimePoint _lastRejectionsLog;
};
}

#endif // ROOFTREE_SESSION_MGR_H
