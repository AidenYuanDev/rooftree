/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#ifndef ROOFTREE_HTTP_SOCKET_H
#define ROOFTREE_HTTP_SOCKET_H

#include "BaseHttpSocket.h"

namespace Rooftree::Net::Http
{
// Plain HTTP; a RequestHandler of its own makes it a server's (examples/http_server.cpp)
class Socket : public BaseSocket<Impl::BoostBeastSocketWrapper>
{
public:
    explicit Socket(IoContextTcpSocket&& socket) : BaseSocket(std::move(socket)) { }
};
}

#endif // ROOFTREE_HTTP_SOCKET_H
