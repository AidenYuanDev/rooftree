/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A thread that runs an io_context of its own and calls a tick every millisecond: what a
 * NetworkThread (TCP sockets) and a Kcp::Server (a UDP port) both run on. It knows
 * nothing of sockets or protocols.
 *
 *   TickThread thread;
 *   socket = Socket(thread.GetIoContext());      // anything bound to it, before or after Start
 *   thread.Start([&] { UpdateEverything(); },     // the thread: every millisecond
 *                [&] { DropEverything(); });      // the thread: once, after the io_context stopped
 *   ...
 *   thread.Stop();                                // any thread
 *   thread.Wait();
 *
 * The next tick is due a millisecond after the last one started, however long it took. Work started
 * on the io_context before Start (an async read) runs once the thread does.
 *
 * Stop doesn't run what is still pending on the io_context: a coroutine waiting on it is destroyed
 * with it (Task.h). An owner with something to finish on the thread
 * posts it and waits for it first.
 */

#ifndef ROOFTREE_TICK_THREAD_H
#define ROOFTREE_TICK_THREAD_H

#include "DeadlineTimer.h"
#include "Duration.h"
#include "Errors.h"
#include "IoContext.h"
#include "Task.h"

#include <atomic>
#include <functional>
#include <memory>
#include <thread>

namespace Rooftree::Net {
class TickThread {
 public:
  static constexpr Milliseconds TickInterval = 1ms;

  // One thread runs the io_context: Asio may leave out some locking
  TickThread() : _ioContext(1), _tickTimer(_ioContext) {}
  TickThread(TickThread const&) = delete;
  TickThread& operator=(TickThread const&) = delete;

  // The owner should have stopped it already: what the thread calls may be gone by now
  ~TickThread() {
    Stop();
    if (_thread)
      Wait();
  }

  // tick and onExit run on the new thread; false if it is
  // already started. Once: a stopped one stays stopped
  bool Start(std::function<void()> tick, std::function<void()> onExit = {}) {
    if (_thread)
      return false;

    _tick = std::move(tick);
    _onExit = std::move(onExit);
    _thread = std::make_unique<std::thread>([this] { Run(); });
    return true;
  }

  // Any thread: no tick after the one running, the io_context stops
  void Stop() {
    _stopped = true;
    _ioContext.stop();
  }

  // After Stop, not on the thread itself
  void Wait() {
    ASSERT(_thread);
    _thread->join();
    _thread = nullptr;
  }

  bool IsStarted() const { return _thread != nullptr; }

  Asio::IoContext& GetIoContext() { return _ioContext; }

 private:
  void Run() {
    TickLoop();
    _ioContext.run();
    if (_onExit)
      _onExit();
  }

  Rooftree::Detached TickLoop() {
    _tickTimer.expires_after(TickInterval);
    while (!_stopped) {
      co_await Rooftree::Await<boost::system::error_code>([this](auto done) {
        _tickTimer.async_wait(std::move(done));
      });
      if (_stopped)
        break;
      // Set before the tick: every millisecond however long a tick takes
      _tickTimer.expires_after(TickInterval);
      _tick();
    }
  }

  std::atomic<bool> _stopped = false;
  std::unique_ptr<std::thread> _thread;
  std::function<void()> _tick;
  std::function<void()> _onExit;
  Asio::IoContext _ioContext;
  Asio::DeadlineTimer _tickTimer;
};
}  // namespace Rooftree::Net

#endif  // ROOFTREE_TICK_THREAD_H
