/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A listening port and the network threads its connections are spread over. Traits names three types:
 * Self (the class deriving from SocketMgr), SocketType and ThreadType (a NetworkThread).
 *
 *   StartNetwork(ioContext, ip, port, threads)   binds, starts the threads, accepts on ioContext
 *   OnSocketOpen(socket)                         the acceptor's thread: limits, CreateSocket, Start
 *   StopNetwork()                                stops accepting and the threads, waits for them
 *
 * A connection goes to the thread with the fewest. Addresses can be blocked or held to a number of
 * connections each (ConnectionLimiter.h); those are refused before anything is made for them.
 */

#ifndef ROOFTREE_SOCKET_MGR_H
#define ROOFTREE_SOCKET_MGR_H

#include "AsyncAcceptor.h"
#include "ConnectionLimiter.h"
#include "Errors.h"
#include "NetworkThread.h"
#include "Socket.h"
#include <boost/asio/ip/tcp.hpp>
#include <memory>

namespace Rooftree::Net
{
template<typename Traits>
class SocketMgr
{
public:
    using Self = typename Traits::Self;
    using SocketType = typename Traits::SocketType;
    using ThreadType = typename Traits::ThreadType;

    SocketMgr(SocketMgr const&) = delete;
    SocketMgr& operator=(SocketMgr const&) = delete;

    virtual ~SocketMgr()
    {
        ASSERT(!_threads && !_acceptor, "StopNetwork before the SocketMgr is destroyed");
    }

    virtual bool StartNetwork(Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int32 threadCount)
    {
        ASSERT(threadCount > 0);
        std::unique_ptr<AsyncAcceptor> acceptor;
        try
        {
            acceptor = std::make_unique<AsyncAcceptor>(ioContext, bindIp, port);
        }
        catch (boost::system::system_error const& e)
        {
            RT_LOG_ERROR("network", "Can't listen on {}:{}: {}", bindIp, port, e.what());
            return false;
        }
        if (!acceptor->Bind())
            return false;

        _acceptor = std::move(acceptor);
        _threadCount = threadCount;
        _threads = static_cast<Self*>(this)->CreateThreads();
        for (int32 i = 0; i < _threadCount; ++i)
            _threads[i].Start();

        _acceptor->AsyncAccept([this] { return SelectThreadWithMinConnections(); },
            [this](IoContextTcpSocket&& socket) { static_cast<Self*>(this)->OnSocketOpen(std::move(socket)); });
        return true;
    }

    virtual void StopNetwork()
    {
        if (_acceptor)
            _acceptor->Close();
        for (int32 i = 0; i < _threadCount; ++i)
            _threads[i].Stop();
        Wait();
        _acceptor = nullptr;
        _threads = nullptr;
        _threadCount = 0;
    }

    void Wait()
    {
        for (int32 i = 0; i < _threadCount; ++i)
            _threads[i].Wait();
    }

    virtual void OnSocketOpen(IoContextTcpSocket&& socket)
    {
        try
        {
            boost::asio::ip::address peer = socket.remote_endpoint().address();
            std::shared_ptr<void> ticket;
            if (_connectionLimiter.IsBlocked(peer))
            {
                RT_LOG_DEBUG("network", "{} is blocked, its connection refused", peer);
                return;
            }
            if (!_connectionLimiter.TryAcquire(peer, ticket))
            {
                RT_LOG_DEBUG("network", "{} has {} connections open already, refused", peer, _connectionLimiter.GetLimit());
                return;
            }

            // An answer goes out whole at once, its last segment isn't held back until the client
            // acknowledges the one before, which the client delays (Nagle against delayed ACK, 40 ms)
            boost::system::error_code ignored;
            socket.set_option(boost::asio::ip::tcp::no_delay(true), ignored);

            ThreadType& thread = ThreadOf(socket);
            std::shared_ptr<SocketType> created = static_cast<Self*>(this)->CreateSocket(std::move(socket));
            created->SetConnectionTicket(std::move(ticket));
            created->Start();
            thread.AddSocket(std::move(created));
        }
        catch (boost::system::system_error const& e)
        {
            // remote_endpoint: the client was gone already
            RT_LOG_DEBUG("network", "Can't take a connection: {}", e.what());
        }
    }

    int32 GetNetworkThreadCount() const { return _threadCount; }

    // Before StartNetwork. At most max connections per client address, 0 is no limit; exempt are
    // addresses in front of many clients (a reverse proxy)
    void SetMaxConnectionsPerAddress(uint32 max, std::vector<boost::asio::ip::address> exempt = {})
    {
        _connectionLimiter.SetLimit(max, std::move(exempt));
    }
    ConnectionLimiter const& GetConnectionLimiter() const { return _connectionLimiter; }
    // Before StartNetwork. Their connections are closed at once
    void SetAddressBlocklist(std::shared_ptr<AddressBlocklist const> blocklist) { _connectionLimiter.SetBlocklist(std::move(blocklist)); }

    Asio::IoContext* SelectThreadWithMinConnections() const
    {
        ThreadType* least = &_threads[0];
        for (int32 i = 1; i < _threadCount; ++i)
            if (_threads[i].GetConnectionCount() < least->GetConnectionCount())
                least = &_threads[i];
        return least->GetIoContext();
    }

protected:
    SocketMgr() = default;

    virtual std::unique_ptr<ThreadType[]> CreateThreads() const { return std::make_unique<ThreadType[]>(_threadCount); }

    // For a socket that takes more than the TCP socket (its service, a TLS context)
    virtual std::shared_ptr<SocketType> CreateSocket(IoContextTcpSocket&& socket) { return std::make_shared<SocketType>(std::move(socket)); }

private:
    // The thread whose io_context the acceptor put the socket on
    ThreadType& ThreadOf(IoContextTcpSocket& socket) const
    {
        for (int32 i = 0; i < _threadCount; ++i)
            if (_threads[i].GetIoContext()->get_executor() == socket.get_executor())
                return _threads[i];
        return _threads[0];
    }

    std::unique_ptr<AsyncAcceptor> _acceptor;
    std::unique_ptr<ThreadType[]> _threads;
    int32 _threadCount = 0;
    ConnectionLimiter _connectionLimiter;
};
}

#endif // ROOFTREE_SOCKET_MGR_H
