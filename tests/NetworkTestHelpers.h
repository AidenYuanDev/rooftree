/*
 * Loopback helpers shared by the network tests: a free port, a blocking client socket with a read
 * timeout, waiting for a condition, and the io_context that owns a server's acceptor.
 */

#ifndef ROOFTREE_NETWORK_TEST_HELPERS_H
#define ROOFTREE_NETWORK_TEST_HELPERS_H

#include "Define.h"
#include "IoContext.h"
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <chrono>
#include <sys/socket.h>
#include <thread>

namespace NetworkTest
{
using namespace std::chrono_literals;
using boost::asio::ip::tcp;

// AsyncAcceptor doesn't expose the port it bound, so let the kernel pick a free one first
inline uint16 FindFreePort()
{
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

// Blocking client socket whose reads fail after a timeout instead of hanging the test run
inline tcp::socket Connect(boost::asio::io_context& io, uint16 port)
{
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
    timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
    setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return socket;
}

template <typename Predicate>
bool WaitFor(Predicate&& predicate, std::chrono::milliseconds timeout = 5s)
{
    auto end = std::chrono::steady_clock::now() + timeout;
    while (!predicate())
    {
        if (std::chrono::steady_clock::now() > end)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// Runs the io_context that owns the acceptor, like the main thread of echo_server/http_server
class ServerContext
{
public:
    ServerContext() : _guard(boost::asio::make_work_guard(_ioContext)), _thread([this] { _ioContext.run(); }) { }

    ~ServerContext() { Stop(); }

    void Stop()
    {
        if (!_thread.joinable())
            return;

        _guard.reset();
        _ioContext.stop();
        _thread.join();
    }

    Rooftree::Asio::IoContext& Get() { return _ioContext; }

private:
    Rooftree::Asio::IoContext _ioContext;
    boost::asio::executor_work_guard<Rooftree::Asio::IoContextExecutor> _guard;
    std::thread _thread;
};

// SocketMgr asserts that StopNetwork() ran before destruction, also stop it when a REQUIRE or a
// client read timeout throws out of the test.
// The acceptor's io_context has to be stopped first (echo_server/http_server do the same: StopNetwork
// runs after ioContext.run() returned): StopNetwork destroys the AsyncAcceptor, and the cancelled
// async_accept handler would otherwise still run on that io_context and touch it.
template <typename Server>
struct StopNetworkGuard
{
    ServerContext& context;
    Server& server;
    ~StopNetworkGuard()
    {
        context.Stop();
        server.StopNetwork();
    }
};

}

#endif // ROOFTREE_NETWORK_TEST_HELPERS_H
