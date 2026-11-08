/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * TLS over a stream, with the members Socket uses (Socket.h), and the server's side of the handshake:
 *
 *   void Start() override { AcceptTls(*this, [](MySocket& socket) { socket.StartReading(); }); }
 *
 * The certificate is the context's (SslContext.h); clients aren't asked for one.
 */

#ifndef ROOFTREE_SSL_STREAM_H
#define ROOFTREE_SSL_STREAM_H

#include "Log.h"
#include "Socket.h"
#include "Task.h"
#include <boost/asio/ssl/stream.hpp>
#include <memory>

namespace Rooftree::Net
{
template<typename WrappedStream = IoContextTcpSocket>
class SslStream
{
public:
    using executor_type = typename WrappedStream::executor_type;

    SslStream(IoContextTcpSocket&& socket, boost::asio::ssl::context& context) : _tls(std::move(socket), context)
    {
        _tls.set_verify_mode(boost::asio::ssl::verify_none);
    }

    executor_type get_executor() { return _tls.get_executor(); }
    bool is_open() const { return _tls.next_layer().is_open(); }
    void close(boost::system::error_code& error) { _tls.next_layer().close(error); }
    void cancel(boost::system::error_code& error) { _tls.next_layer().cancel(error); }

    // TLS's close_notify, then the TCP end of stream
    void shutdown(boost::asio::socket_base::shutdown_type what, boost::system::error_code& error)
    {
        _tls.shutdown(error);
        _tls.next_layer().shutdown(what, error);
    }

    template<typename Buffers, typename Token>
    auto async_read_some(Buffers const& buffers, Token&& token) { return _tls.async_read_some(buffers, std::forward<Token>(token)); }

    template<typename Buffers>
    std::size_t write_some(Buffers const& buffers, boost::system::error_code& error) { return _tls.write_some(buffers, error); }

    template<typename Token>
    auto async_wait(boost::asio::socket_base::wait_type type, Token&& token) { return _tls.next_layer().async_wait(type, std::forward<Token>(token)); }

    template<typename Option>
    void set_option(Option const& option, boost::system::error_code& error) { _tls.next_layer().set_option(option, error); }

    IoContextTcpSocket::endpoint_type remote_endpoint() const { return _tls.next_layer().remote_endpoint(); }

    template<typename Token>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, Token&& token)
    {
        return _tls.async_handshake(type, std::forward<Token>(token));
    }

private:
    boost::asio::ssl::stream<WrappedStream> _tls;
};

// The server's handshake on socket's stream, then then(socket) on its network thread. A failed one
// closes the socket. The coroutine doesn't keep the socket: one closed or gone meanwhile (its deadline
// passed) is destroyed, which ends the handshake
template<typename SocketType, typename Then>
Rooftree::Detached AcceptTls(SocketType& socket, Then then)
{
    std::weak_ptr<SocketType> weak = std::static_pointer_cast<SocketType>(socket.shared_from_this());
    auto [error] = co_await Rooftree::Await<boost::system::error_code>([&](auto done)
    {
        socket.underlying_stream().async_handshake(boost::asio::ssl::stream_base::server, std::move(done));
    });

    // Not socket from here on, it may be gone
    std::shared_ptr<SocketType> self = weak.lock();
    if (!self)
        co_return;
    if (error)
    {
        // A scanner, an old TLS version, a client gone halfway: the client's doing, not an error of ours
        RT_LOG_DEBUG("network", "{}:{} TLS handshake failed: {}", self->GetRemoteIpAddress(), self->GetRemotePort(), error.message());
        self->CloseSocket();
        co_return;
    }
    then(*self);
}
}

#endif // ROOFTREE_SSL_STREAM_H
