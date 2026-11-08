/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A thread with an io_context of its own and the sockets on it: their reads and writes complete here,
 * and every millisecond Update() calls each socket's Update() (which writes what was queued) and lets
 * go of the closed ones. SocketMgr spreads new connections over several of these.
 *
 * DerivedThread is the class deriving from it, for SocketAdded and SocketRemoved.
 */

#ifndef ROOFTREE_NETWORK_THREAD_H
#define ROOFTREE_NETWORK_THREAD_H

#include "DeadlineTimer.h"
#include "Define.h"
#include "Duration.h"
#include "Errors.h"
#include "IoContext.h"
#include "Log.h"
#include "Task.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace Rooftree::Net
{
template<typename SocketType, typename DerivedThread>
class NetworkThread
{
public:
    // One thread runs the io_context: Asio may leave out some locking
    NetworkThread() : _ioContext(1), _updateTimer(_ioContext) { }
    NetworkThread(NetworkThread const&) = delete;
    NetworkThread& operator=(NetworkThread const&) = delete;

    virtual ~NetworkThread()
    {
        Stop();
        if (_thread)
            Wait();
    }

    bool Start()
    {
        if (_thread)
            return false;
        _thread = std::make_unique<std::thread>([this] { Run(); });
        return true;
    }

    // Any thread
    void Stop()
    {
        _stopped = true;
        _ioContext.stop();
    }

    void Wait()
    {
        ASSERT(_thread);
        _thread->join();
        _thread = nullptr;
    }

    // Any thread, so a little out of date
    int32 GetConnectionCount() const { return _connections; }

    // Any thread (the acceptor's): the thread takes it in its next update
    void AddSocket(std::shared_ptr<SocketType> socket)
    {
        std::scoped_lock lock(_incomingMutex);
        ++_connections;
        static_cast<DerivedThread*>(this)->SocketAdded(_incoming.emplace_back(std::move(socket)));
    }

    Asio::IoContext* GetIoContext() { return &_ioContext; }

protected:
    virtual void SocketAdded(std::shared_ptr<SocketType> const& /*socket*/) { }
    virtual void SocketRemoved(std::shared_ptr<SocketType> const& /*socket*/) { }

private:
    void Run()
    {
        RT_LOG_DEBUG("network", "Network thread starting");
        UpdateLoop();
        _ioContext.run();
        RT_LOG_DEBUG("network", "Network thread exits");
        _incoming.clear();
        _sockets.clear();
    }

    Rooftree::Detached UpdateLoop()
    {
        _updateTimer.expires_after(1ms);
        while (!_stopped)
        {
            co_await Rooftree::Await<boost::system::error_code>([this](auto done) { _updateTimer.async_wait(std::move(done)); });
            if (_stopped)
                break;
            // Set before the update: every millisecond however long an update with many sockets takes
            _updateTimer.expires_after(1ms);
            Update();
        }
    }

    void Update()
    {
        {
            std::scoped_lock lock(_incomingMutex);
            // Closing ones too: one that answered its first request with "Connection: close" still
            // has that answer to write, Update() lets go of it once written
            for (std::shared_ptr<SocketType>& socket : _incoming)
                _sockets.push_back(std::move(socket));
            _incoming.clear();
        }

        std::erase_if(_sockets, [this](std::shared_ptr<SocketType> const& socket)
        {
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
    std::atomic<bool> _stopped = false;
    std::unique_ptr<std::thread> _thread;
    std::vector<std::shared_ptr<SocketType>> _sockets;
    std::mutex _incomingMutex;
    std::vector<std::shared_ptr<SocketType>> _incoming;
    Asio::IoContext _ioContext;
    Asio::DeadlineTimer _updateTimer;
};
}

#endif // ROOFTREE_NETWORK_THREAD_H
