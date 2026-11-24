/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A thread with an io_context of its own and the sockets on it: their reads and writes complete
 * here, and every millisecond Update() calls each socket's Update() (which writes what was queued)
 * and lets go of the closed ones. SocketMgr spreads new connections over several of these. The
 * thread, its io_context and the millisecond are a TickThread's.
 *
 * DerivedThread is the class deriving from it, for SocketAdded and SocketRemoved.
 */

#ifndef ROOFTREE_NETWORK_THREAD_H
#define ROOFTREE_NETWORK_THREAD_H

#include "Define.h"
#include "IoContext.h"
#include "Log.h"
#include "TickThread.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace Rooftree::Net {
template <typename SocketType, typename DerivedThread>
class NetworkThread {
 public:
  NetworkThread() = default;
  NetworkThread(NetworkThread const&) = delete;
  NetworkThread& operator=(NetworkThread const&) = delete;

  virtual ~NetworkThread() {
    Stop();
    if (_thread.IsStarted())
      Wait();
  }

  bool Start() {
    if (_thread.IsStarted())
      return false;
    RT_LOG_DEBUG("network", "Network thread starting");
    return _thread.Start([this] { Update(); }, [this] { Exit(); });
  }

  // Any thread
  void Stop() { _thread.Stop(); }

  void Wait() { _thread.Wait(); }

  // Any thread, so a little out of date
  int32 GetConnectionCount() const { return _connections; }

  // Any thread (the acceptor's): the thread takes it in its next update
  void AddSocket(std::shared_ptr<SocketType> socket) {
    std::scoped_lock lock(_incomingMutex);
    ++_connections;
    static_cast<DerivedThread*>(this)->SocketAdded(_incoming.emplace_back(std::move(socket)));
  }

  Asio::IoContext* GetIoContext() { return &_thread.GetIoContext(); }

 protected:
  virtual void SocketAdded(std::shared_ptr<SocketType> const& /*socket*/) {}
  virtual void SocketRemoved(std::shared_ptr<SocketType> const& /*socket*/) {}

 private:
  // The thread, once its io_context stopped
  void Exit() {
    RT_LOG_DEBUG("network", "Network thread exits");
    _incoming.clear();
    _sockets.clear();
  }

  void Update() {
    {
      std::scoped_lock lock(_incomingMutex);
      // Closing ones too: one that answered its first request with "Connection: close" still
      // has that answer to write, Update() lets go of it once written
      for (std::shared_ptr<SocketType>& socket : _incoming)
        _sockets.push_back(std::move(socket));
      _incoming.clear();
    }

    std::erase_if(_sockets, [this](std::shared_ptr<SocketType> const& socket) {
      if (socket->Update())
        return false;
      if (socket->IsOpen())
        socket->CloseSocket();
      static_cast<DerivedThread*>(this)->SocketRemoved(socket);
      --_connections;
      return true;
    });
  }

  std::atomic<int32> _connections = 0;
  std::vector<std::shared_ptr<SocketType>> _sockets;
  std::mutex _incomingMutex;
  std::vector<std::shared_ptr<SocketType>> _incoming;
  TickThread _thread;  // last: its io_context goes first, after Exit let go of the sockets on it
};
}  // namespace Rooftree::Net

#endif  // ROOFTREE_NETWORK_THREAD_H
