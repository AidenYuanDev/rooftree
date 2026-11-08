/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A coroutine that returns a T: it starts when awaited, and whoever awaited it goes on where it finished,
 * without another stack frame (symmetric transfer). An exception it lets out comes out of the co_await.
 * It knows nothing of threads: it runs on the thread that resumes it, the awaited thing decides which
 * one that is (Session::OnWorker comes back to the main thread).
 *
 * The first one of a chain has nobody awaiting it, Session::Spawn runs those. Destroying a Task destroys
 * its coroutine with whatever it holds, also one that is suspended: it just never goes on.
 *
 * For the network layer, which needs a coroutine per connection at the cost of a callback:
 *  - Detached is a coroutine nobody awaits: it starts right away on the calling thread and frees itself
 *    at its end. What it lets out ends the program, so it catches what can be expected
 *  - Await turns an operation that takes a callback into something to co_await:
 *
 *      auto [error, bytes] = co_await Await<error_code, std::size_t>([&](auto done) { socket.async_read_some(buffer, std::move(done)); });
 *
 *    The coroutine goes on in that callback, so on the thread that runs it. It has to be awaited by the
 *    Detached itself, not by a Task under it. If the operation never calls back and is destroyed (its
 *    io_context goes away), the coroutine is destroyed with it.
 *
 * WhenCalled is for a callback API of the thread the coroutine runs on, which may call back right
 * away, later or never: a Task waits on it too, and goes on with nothing for never.
 */

#ifndef ROOFTREE_TASK_H
#define ROOFTREE_TASK_H

#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <tuple>
#include <utility>

namespace Rooftree
{
template<typename T = void>
class Task;

namespace Detail
{
class TaskPromiseBase
{
public:
    std::suspend_always initial_suspend() noexcept { return {}; }

    auto final_suspend() noexcept
    {
        struct Continue
        {
            bool await_ready() noexcept { return false; }
            void await_resume() noexcept { }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<>) noexcept
            {
                if (Next)
                    return Next;
                return std::noop_coroutine();
            }

            std::coroutine_handle<> Next;
        };
        return Continue{ _continuation };
    }

    void unhandled_exception() noexcept { _error = std::current_exception(); }
    void SetContinuation(std::coroutine_handle<> continuation) noexcept { _continuation = continuation; }

protected:
    void RethrowIfFailed()
    {
        if (_error)
            std::rethrow_exception(_error);
    }

private:
    std::coroutine_handle<> _continuation;
    std::exception_ptr _error;
};

template<typename T>
class TaskPromise : public TaskPromiseBase
{
public:
    Task<T> get_return_object() noexcept;

    // By value: co_return {} and what converts to T
    void return_value(T value) { _value.emplace(std::move(value)); }

    T TakeResult()
    {
        RethrowIfFailed();
        return std::move(*_value);
    }

private:
    std::optional<T> _value;
};

template<>
class TaskPromise<void> : public TaskPromiseBase
{
public:
    Task<void> get_return_object() noexcept;
    void return_void() noexcept { }
    void TakeResult() { RethrowIfFailed(); }
};
}

template<typename T>
class [[nodiscard]] Task
{
public:
    using promise_type = Detail::TaskPromise<T>;

    explicit Task(std::coroutine_handle<promise_type> handle) noexcept : _handle(handle) { }
    Task(Task&& other) noexcept : _handle(std::exchange(other._handle, nullptr)) { }
    Task& operator=(Task&& other) noexcept
    {
        if (this != &other)
        {
            if (_handle)
                _handle.destroy();
            _handle = std::exchange(other._handle, nullptr);
        }
        return *this;
    }
    Task(Task const&) = delete;
    Task& operator=(Task const&) = delete;
    ~Task()
    {
        if (_handle)
            _handle.destroy();
    }

    bool await_ready() const noexcept { return false; }

    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiting) noexcept
    {
        _handle.promise().SetContinuation(awaiting);
        return _handle;
    }

    T await_resume() { return _handle.promise().TakeResult(); }

private:
    std::coroutine_handle<promise_type> _handle;
};

namespace Detail
{
template<typename T>
Task<T> TaskPromise<T>::get_return_object() noexcept
{
    return Task<T>(std::coroutine_handle<TaskPromise<T>>::from_promise(*this));
}

inline Task<void> TaskPromise<void>::get_return_object() noexcept
{
    return Task<void>(std::coroutine_handle<TaskPromise<void>>::from_promise(*this));
}
}

struct Detached
{
    struct promise_type
    {
        Detached get_return_object() noexcept { return {}; }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_never final_suspend() noexcept { return {}; }
        void return_void() noexcept { }
        void unhandled_exception() noexcept { std::terminate(); }
    };
};

namespace Detail
{
// Who goes on when the operation calls back, or is destroyed with the operation if it never does
class OwnedCoroutine
{
public:
    explicit OwnedCoroutine(std::coroutine_handle<> coroutine) noexcept : _coroutine(coroutine) { }
    OwnedCoroutine(OwnedCoroutine&& other) noexcept : _coroutine(std::exchange(other._coroutine, nullptr)) { }
    OwnedCoroutine& operator=(OwnedCoroutine&&) = delete;
    ~OwnedCoroutine()
    {
        if (_coroutine)
            _coroutine.destroy();
    }

    void Resume() { std::exchange(_coroutine, nullptr).resume(); }

private:
    std::coroutine_handle<> _coroutine;
};
}

template<typename... Results, typename Initiate>
auto Await(Initiate initiate)
{
    class Awaiter
    {
    public:
        explicit Awaiter(Initiate&& initiate) : _initiate(std::move(initiate)) { }

        bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> coroutine)
        {
            _initiate([this, owner = Detail::OwnedCoroutine(coroutine)](Results... results) mutable
            {
                _results.emplace(std::move(results)...);
                owner.Resume();
            });
        }

        std::tuple<Results...> await_resume() { return std::move(*_results); }

    private:
        Initiate _initiate;
        std::optional<std::tuple<Results...>> _results;
    };

    return Awaiter(std::move(initiate));
}

// co_await: initiate(callback) hands callback, copyable, to an API that calls it at most once, now or
// later, on the thread this coroutine runs on. The co_await gives what it was called with, or nothing
// once every copy of it was destroyed uncalled (the API answered the request itself). For a Task as
// well: the coroutine always goes on
template<typename... Args, typename Initiate>
auto WhenCalled(Initiate initiate)
{
    using Result = std::optional<std::tuple<Args...>>;

    struct State
    {
        Result Value;
        std::coroutine_handle<> Waiting;     // set once suspended
        bool Done = false;

        void Finish()
        {
            Done = true;
            if (Waiting)
                std::exchange(Waiting, nullptr).resume();
        }
    };

    // Who has a copy of the callback: the last one going away uncalled finishes it with nothing
    struct Guard
    {
        explicit Guard(std::shared_ptr<State> state) : Target(std::move(state)) { }
        Guard(Guard const&) = delete;
        Guard& operator=(Guard const&) = delete;
        ~Guard()
        {
            if (!Target->Done)
                Target->Finish();
        }

        std::shared_ptr<State> Target;
    };

    class Awaiter
    {
    public:
        explicit Awaiter(Initiate&& initiate) : _initiate(std::move(initiate)), _state(std::make_shared<State>()) { }
        Awaiter(Awaiter const&) = delete;
        Awaiter& operator=(Awaiter const&) = delete;
        // Also with the coroutine destroyed while it waits: a call afterwards does nothing
        ~Awaiter()
        {
            _state->Waiting = nullptr;
            _state->Done = true;
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> coroutine)
        {
            {
                auto guard = std::make_shared<Guard>(_state);
                _initiate([guard](Args... args)
                {
                    State& state = *guard->Target;
                    if (state.Done)
                        return;
                    state.Value.emplace(std::move(args)...);
                    state.Finish();
                });
            }
            // Called or dropped already: go on without suspending
            if (_state->Done)
                return false;
            _state->Waiting = coroutine;
            return true;
        }

        Result await_resume() { return std::move(_state->Value); }

    private:
        Initiate _initiate;
        std::shared_ptr<State> _state;
    };

    return Awaiter(std::move(initiate));
}
}

#endif // ROOFTREE_TASK_H
