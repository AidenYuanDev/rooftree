/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A stream that is TLS or plain, decided per connection when it is accepted. Picking one at compile
 * time (Http::Socket or Http::SslSocket) would make one service need two socket types, two
 * SocketMgr and two copies of everything built on top of them. A server that listens on 80 and 443
 * with the same routes, the same WebSocket upgrade, uses one socket type with this stream instead.
 *
 * It has the operations Socket<Stream> needs and async_handshake, forwarded to whichever stream it
 * holds; a plain one never handshakes.
 */

#ifndef ROOFTREE_OPTIONAL_SSL_STREAM_H
#define ROOFTREE_OPTIONAL_SSL_STREAM_H

#include "Errors.h"
#include "SslStream.h"
#include <variant>

namespace Rooftree::Net
{
template<class WrappedStream = IoContextTcpSocket>
class OptionalSslStream
{
public:
    using executor_type = typename WrappedStream::executor_type;

    // sslContext null: plain. Otherwise it must outlive the stream
    OptionalSslStream(IoContextTcpSocket&& socket, boost::asio::ssl::context* sslContext)
        : _stream(sslContext ? Variant(std::in_place_index<1>, std::move(socket), *sslContext) : Variant(std::in_place_index<0>, std::move(socket)))
    {
    }

    bool IsSsl() const { return _stream.index() == 1; }

    executor_type get_executor()
    {
        return std::visit([](auto& stream) { return stream.get_executor(); }, _stream);
    }

    bool is_open() const
    {
        return std::visit([](auto const& stream) { return stream.is_open(); }, _stream);
    }

    void close(boost::system::error_code& error)
    {
        std::visit([&](auto& stream) { stream.close(error); }, _stream);
    }

    void cancel(boost::system::error_code& error)
    {
        std::visit([&](auto& stream) { stream.cancel(error); }, _stream);
    }

    void shutdown(boost::asio::socket_base::shutdown_type what, boost::system::error_code& shutdownError)
    {
        std::visit([&](auto& stream) { stream.shutdown(what, shutdownError); }, _stream);
    }

    template<typename MutableBufferSequence, typename CompletionToken>
    auto async_read_some(MutableBufferSequence const& buffers, CompletionToken&& token)
    {
        return std::visit([&](auto& stream) { return stream.async_read_some(buffers, std::forward<CompletionToken>(token)); }, _stream);
    }

    template<typename ConstBufferSequence>
    std::size_t write_some(ConstBufferSequence const& buffers, boost::system::error_code& error)
    {
        return std::visit([&](auto& stream) { return stream.write_some(buffers, error); }, _stream);
    }

    template<typename CompletionToken>
    auto async_wait(boost::asio::socket_base::wait_type type, CompletionToken&& token)
    {
        return std::visit([&](auto& stream) { return stream.async_wait(type, std::forward<CompletionToken>(token)); }, _stream);
    }

    template<typename SettableSocketOption>
    void set_option(SettableSocketOption const& option, boost::system::error_code& error)
    {
        std::visit([&](auto& stream) { stream.set_option(option, error); }, _stream);
    }

    IoContextTcpSocket::endpoint_type remote_endpoint() const
    {
        return std::visit([](auto const& stream) { return stream.remote_endpoint(); }, _stream);
    }

    // For AcceptTls (SslStream.h), only on a TLS stream
    template<typename CompletionToken>
    auto async_handshake(boost::asio::ssl::stream_base::handshake_type type, CompletionToken&& token)
    {
        ASSERT(IsSsl());
        return std::get<1>(_stream).async_handshake(type, std::forward<CompletionToken>(token));
    }

private:
    using Variant = std::variant<WrappedStream, SslStream<WrappedStream>>;
    Variant _stream;
};
}

#endif // ROOFTREE_OPTIONAL_SSL_STREAM_H
