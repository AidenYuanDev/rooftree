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
    struct promise_type
    {
        promise_type(Session& session, Rooftree::Task<void> const&) : Owner(session) { }

        SpawnedCoroutine get_return_object() { return { std::coroutine_handle<promise_type>::from_promise(*this) }; }
        std::suspend_always initial_suspend() noexcept { return {}; }

        auto final_suspend() noexcept
        {
            struct Finish
            {
                bool await_ready() noexcept { return false; }
                void await_resume() noexcept { }

                void await_suspend(std::coroutine_handle<promise_type> self) noexcept
                {
                    self.promise().Owner._coroutines.erase(self.address());
                    self.destroy();
                }
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
    std::coroutine_handle<SpawnedCoroutine::promise_type> handle = RunSpawned(*this, std::move(task)).Handle;
    _coroutines.insert(handle.address());
    handle.resume();
}

void Session::DestroyCoroutines()
{
    // Destroying one may release what another waits for, but nothing resumes on this thread now
    std::unordered_set<void*> coroutines = std::exchange(_coroutines, {});
    for (void* coroutine : coroutines)
        std::coroutine_handle<>::from_address(coroutine).destroy();
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
