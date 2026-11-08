/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The main-thread object of one client, which owns the client's
 * queued messages and the asynchronous work started for it. SessionMgr creates sessions by key on the
 * first message for that key (an HTTP client's address, a WebSocket's account, ...) and removes them
 * once closed or idle.
 *
 * Everything here runs on the main thread, so a session's state needs no locks. Work that would
 * block it goes elsewhere and comes back:
 *  - database queries: AddQueryCallback(), the callback runs in a later update of this session
 *  - CPU heavy work (password hashing, sending mail), several dependent queries: a coroutine. Spawn()
 *    runs a Task on the main thread, `co_await OnWorker(work)` runs work() on SessionMgr's thread pool
 *    and goes on on the main thread, in a later update, with its result
 * Either keeps the session alive until it is back, so the callback or the coroutine may use the
 * session freely. This used to be RunOnWorker(work, done), callbacks: a coroutine reads top
 * to bottom, its early returns and dependent steps included.
 *
 * What work throws comes out of the co_await. The session outlives what it spawned while that waits
 * for a worker; when the session goes, the coroutines still there are destroyed. After a co_await the
 * session is the same but anything else may have changed: a game's player may have left, a reference
 * into a cache may be gone, check again what the code relies on. Not for what runs per packet of a
 * game or per tick: a coroutine allocates its frame.
 *
 * Write a coroutine as a function taking its arguments by value. A reference or a string_view argument
 * is gone after the first co_await unless what it refers to outlives the coroutine (the session, a
 * console's CommandHandler under its Spawn); the captures of a lambda that is itself the coroutine are
 * gone with the lambda.
 *
 * An exception out of a message, a callback or a worker's work is caught and logged, and
 * only what threw goes: the message (an HTTP request is answered 500), the session (closed), the
 * coroutine (it ends there, logged). The game and every other session go on: one process serves them all.
 */

#ifndef ROOFTREE_SERVER_SESSION_H
#define ROOFTREE_SERVER_SESSION_H

#include "AntiDos.h"
#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "QueryCallback.h"
#include "ServerMessage.h"
#include "Task.h"
#include <coroutine>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <variant>

namespace Rooftree::Server
{
class SessionMgr;

class Session
{
public:
    Session(SessionMgr& sessionMgr, std::string key, boost::asio::ip::address remoteAddress);
    Session(Session const&) = delete;
    Session& operator=(Session const&) = delete;
    virtual ~Session();

    std::string const& GetKey() const { return _key; }
    // Its class's name, "" the default class (SessionMgr)
    std::string const& GetSessionClass() const;
    // Of the message that created the session
    boost::asio::ip::address const& GetRemoteAddress() const { return _remoteAddress; }
    SessionMgr& GetSessionMgr() const { return _sessionMgr; }

    std::size_t GetQueuedMessageCount() const { return _queue.size(); }

    void AddQueryCallback(QueryCallback&& callback);

    // Runs task now, up to its first co_await that waits. What it lets out is logged, the session goes on
    void Spawn(Rooftree::Task<void> task);

    // co_await: work() on a worker thread, the coroutine goes on on the main thread with its result
    template<typename Work>
    [[nodiscard]] auto OnWorker(Work work);

    // Removed at the end of this update, queued messages are rejected with SessionClosed
    void Close() { _closed = true; }
    bool IsClosed() const { return _closed; }

protected:
    // Every SessionMgr update, before the session's messages
    virtual void Update(uint32 /*diff*/) { }

    // False holds the queued messages back, e.g. while the session loads its account. Asked before
    // every message, so a message may hold back the ones after it (the console: until it is done)
    virtual bool CanProcessMessages() const { return true; }

    // An idle session (nothing queued or pending, no message for SessionIdleTimeout) is removed only
    // if this agrees, e.g. a WebSocket session stays while its connection is open
    virtual bool CanBeRemoved() const { return true; }

    // A message over its AntiDOS limit, before the policy is applied
    virtual void OnAntiDosViolation(Message const& /*message*/, AntiDosPolicy /*policy*/) { }

    // Right before the session is destroyed
    virtual void OnRemoved() { }

private:
    friend class SessionMgr;

    class SpawnedCoroutine;
    static SpawnedCoroutine RunSpawned(Session& session, Rooftree::Task<void> task);
    void DestroyCoroutines();

    void PostWork(std::function<void()> work);
    void PostCompletion(std::function<void(Session&)> completion);
    bool HasPendingWork() const { return _pendingWork > 0 || !_queryProcessor.Empty(); }
    std::size_t GetPendingWorkCount() const { return _pendingWork + _queryProcessor.Size(); }

    SessionMgr& _sessionMgr;
    std::string _key;
    boost::asio::ip::address _remoteAddress;
    uint32 _classIndex = 0;

    std::deque<std::unique_ptr<Message>> _queue;
    AntiDosCounter _antiDos;
    uint32 _antiDosPeriod = 0;      // SessionMgr's summary it was last counted in
    TimePoint _rejectionsSince;     // AntiDosSettings::BlockAfter: rejections in the window from then
    uint32 _rejectionsInWindow = 0;
    QueryCallbackProcessor _queryProcessor;
    uint32 _pendingWork = 0;
    std::unordered_set<void*> _coroutines;  // what Spawn() runs, by its handle's address, until it ends
    TimePoint _lastActivity;
    bool _closed = false;
};

// What an exception says, for a log
std::string DescribeException(std::exception_ptr error);

template<typename Work>
auto Session::OnWorker(Work work)
{
    using Result = std::invoke_result_t<Work&>;
    using Stored = std::conditional_t<std::is_void_v<Result>, std::monostate, Result>;

    class Awaiter
    {
    public:
        Awaiter(Session& session, Work&& work) : _session(session), _work(std::move(work)) { }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> coroutine)
        {
            ++_session._pendingWork;
            _session.PostWork([this, coroutine]
            {
                try
                {
                    if constexpr (std::is_void_v<Result>)
                        _work();
                    else
                        _result.emplace(_work());
                }
                catch (...)
                {
                    _error = std::current_exception();
                }

                // The frame this awaiter is in waits for this: not touched after it
                _session.PostCompletion([coroutine](Session&) { coroutine.resume(); });
            });
        }

        Result await_resume()
        {
            if (_error)
                std::rethrow_exception(_error);
            if constexpr (!std::is_void_v<Result>)
                return std::move(*_result);
        }

    private:
        Session& _session;
        Work _work;
        std::optional<Stored> _result;
        std::exception_ptr _error;
    };

    return Awaiter(*this, std::move(work));
}
}

#endif // ROOFTREE_SERVER_SESSION_H
