/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "ServerSession.h"
#include "Log.h"
#include "SessionMgr.h"

namespace Rooftree::Server
{
Session::Session(SessionMgr& sessionMgr, std::string key, boost::asio::ip::address remoteAddress)
    : _sessionMgr(sessionMgr), _key(std::move(key)), _remoteAddress(std::move(remoteAddress)), _lastActivity(TimePoint::clock::now())
{
}

// What Spawn() runs: nobody awaits it, the session holds it and it takes itself out when it ends
class Session::SpawnedCoroutine
{
public:
    struct promise_type : SpawnedLink
    {
        promise_type(Session& session, Rooftree::Task<void> const&) : Owner(session)
        {
            SpawnedLink& head = session._coroutines;
            Prev = head.Prev;
            Next = &head;
            head.Prev->Next = this;
            head.Prev = this;
        }

        // At its end or destroyed while it waits
        ~promise_type()
        {
            Prev->Next = Next;
            Next->Prev = Prev;
        }

        SpawnedCoroutine get_return_object() { return { std::coroutine_handle<promise_type>::from_promise(*this) }; }
        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept
        {
            struct Finish
            {
                bool await_ready() noexcept { return false; }
                void await_resume() noexcept { }

                void await_suspend(std::coroutine_handle<promise_type> self) noexcept { self.destroy(); }
            };
            return Finish{};
        }

        void return_void() noexcept { }

        void unhandled_exception() noexcept
        {
            RT_LOG_ERROR("server", "Session {}: a coroutine threw: {}", Owner.GetKey(), DescribeException(std::current_exception()));
        }

        Session& Owner;
    };

    std::coroutine_handle<promise_type> Handle;
};

Session::SpawnedCoroutine Session::RunSpawned(Session& /*session*/, Rooftree::Task<void> task)
{
    co_await task;
}

void Session::Spawn(Rooftree::Task<void> task)
{
    RunSpawned(*this, std::move(task)).Handle.resume();
}

void Session::DestroyCoroutines()
{
    // One at a time from the head: destroying one may let another one go on (a WhenCalled whose
    // callback it held) and end, which takes that one out of the list
    using Promise = SpawnedCoroutine::promise_type;
    while (_coroutines.Next != &_coroutines)
        std::coroutine_handle<Promise>::from_promise(*static_cast<Promise*>(_coroutines.Next)).destroy();
}

Session::~Session()
{
    DestroyCoroutines();
}

std::string const& Session::GetSessionClass() const
{
    return _sessionMgr.GetClass(*this).Name;
}

void Session::AddQueryCallback(QueryCallback&& callback)
{
    _queryProcessor.AddCallback(std::move(callback));
}

void Session::PostWork(std::function<void()> work)
{
    _sessionMgr._workers->PostWork(std::move(work));
}

void Session::PostCompletion(std::function<void(Session&)> completion)
{
    _sessionMgr._completions.add({ .Target = this, .Callback = std::move(completion) });
}

std::string DescribeException(std::exception_ptr error)
{
    try
    {
        std::rethrow_exception(error);
    }
    catch (std::exception const& e)
    {
        return e.what();
    }
    catch (...)
    {
        return "not a std::exception";
    }
}
}
