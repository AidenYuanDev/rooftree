/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A listening TCP socket. AsyncAccept(select, accepted) accepts until Close(): each connection onto
 * the io_context select() picks (the network thread with the fewest connections), then hands it to
 * accepted(socket). A coroutine on the acceptor's own io_context does it.
 */

#ifndef ROOFTREE_ASYNC_ACCEPTOR_H
#define ROOFTREE_ASYNC_ACCEPTOR_H

#include "Concepts.h"
#include "IoContext.h"
#include "IpAddress.h"
#include "Log.h"
#include "Socket.h"
#include "Task.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <atomic>
#include <thread>

namespace Rooftree::Net
{
class AsyncAcceptor
{
public:
    AsyncAcceptor(Asio::IoContext& ioContext, std::string const& bindIp, uint16 port)
        : _ioContext(ioContext), _acceptor(ioContext), _endpoint(make_address(bindIp), port) { }

    // Opens, binds and listens; false (logged) when it can't
    bool Bind()
    {
        boost::system::error_code error;
        auto failed = [&](char const* what)
        {
            if (!error)
                return false;
            RT_LOG_ERROR("network", "Can't listen on {}:{}: {}: {}", _endpoint.address(), _endpoint.port(), what, error.message());
            return true;
        };

        _acceptor.open(_endpoint.protocol(), error);
        if (failed("open"))
            return false;
        // A restart binds the port again at once, without waiting for the old connections' TIME_WAIT
        _acceptor.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true), error);
        if (failed("reuse_address"))
            return false;
        // :: takes IPv4 too, where the system's default is IPv6 only (some BSDs)
        if (_endpoint.protocol() == boost::asio::ip::tcp::v6())
        {
            _acceptor.set_option(boost::asio::ip::v6_only(false), error);
            if (failed("v6_only"))
                return false;
        }
        _acceptor.bind(_endpoint, error);
        if (failed("bind"))
            return false;
        _acceptor.listen(boost::asio::socket_base::max_listen_connections, error);
        return !failed("listen");
    }

    template<invocable_r<Asio::IoContext*> Select, std::invocable<IoContextTcpSocket&&> Accepted>
    void AsyncAccept(Select select, Accepted accepted)
    {
        ++_loops;
        AcceptLoop(std::move(select), std::move(accepted));
    }

    // Stops accepting and waits for the loop to see it: it uses this acceptor and the network threads'
    // io_contexts, which go next. When nothing runs the acceptor's io_context anymore (it was stopped
    // first), runs it here until the loop is done
    void Close()
    {
        if (_closed.exchange(true))
            return;

        boost::system::error_code ignored;
        _acceptor.close(ignored);
        for (int i = 0; _loops > 0 && i < 5000; ++i)
        {
            if (_ioContext.stopped())
            {
                _ioContext.restart();
                _ioContext.poll();
                _ioContext.stop();
            }
            else
                std::this_thread::sleep_for(1ms);
        }
    }

private:
    template<typename Select, typename Accepted>
    Rooftree::Detached AcceptLoop(Select select, Accepted accepted)
    {
        while (!_closed)
        {
            Asio::IoContext* target = select();
            auto [error, socket] = co_await Rooftree::Await<boost::system::error_code, IoContextTcpSocket>([&](auto done)
            {
                _acceptor.async_accept(target->get_executor(), std::move(done));
            });
            if (error)
                continue;

            try
            {
                socket.non_blocking(true);
                accepted(std::move(socket));
            }
            catch (boost::system::system_error const& e)
            {
                RT_LOG_INFO("network", "Can't take an accepted connection: {}", e.what());
            }
        }
        --_loops;
    }

    Asio::IoContext& _ioContext;
    boost::asio::basic_socket_acceptor<boost::asio::ip::tcp, IoContextTcpSocket::executor_type> _acceptor;
    boost::asio::ip::tcp::endpoint _endpoint;
    std::atomic<bool> _closed = false;
    std::atomic<uint32> _loops = 0;
};
}

#endif // ROOFTREE_ASYNC_ACCEPTOR_H
