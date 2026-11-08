/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_HTTP_SSL_SOCKET_H
#define ROOFTREE_HTTP_SSL_SOCKET_H

#include "BaseHttpSocket.h"
#include "SslStream.h"

namespace Rooftree::Net::Http
{
// HTTPS: the TLS handshake, then HTTP. sslContext has to outlive the socket
class SslSocket : public BaseSocket<SslStream<Impl::BoostBeastSocketWrapper>>
{
public:
    SslSocket(IoContextTcpSocket&& socket, boost::asio::ssl::context& sslContext) : BaseSocket(std::move(socket), sslContext) { }

    void Start() override
    {
        AcceptTls(*this, [](SslSocket& socket) { socket.StartHttp(); });
    }
};
}

#endif // ROOFTREE_HTTP_SSL_SOCKET_H
