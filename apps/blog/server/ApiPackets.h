/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The operations of the API (ApiOperations.h) as packets on /ws (Packet.h), for clients of our own:
 * a binary message with an operation's opcode and its request table (schema/Admin.fbs, Reader.fbs)
 * gets its response table, or an Error, with the same request id.
 *
 * The operations work in FlatBuffers (ApiCall.h): a packet's table is what the operation reads, its
 * answer the table it built, nothing converted. The route of the same operation gets there through
 * JSON (ApiJson.h), so a packet does exactly what its route does, with the same checks, limits (the
 * route's message type) and logs.
 *
 * Who calls is the login of the connection's handshake (its session cookie, BlogWebSocket.h), checked
 * per packet as the operation's row says: anyone may call a reader's read, with a login or without;
 * else 401 unauthenticated without one or once it has ended, 403 banned (but for what a banned
 * account may still do), 403 forbidden for an operation of the admin's that a moderator sent. The handshake's Origin check stands in for CSRF's:
 * another site's page can't open the socket with the admin's cookie, so a client of our own sends our
 * Origin as a browser would.
 */

#ifndef BLOG_API_PACKETS_H
#define BLOG_API_PACKETS_H

namespace Rooftree::Server
{
class WebSocketRouter;
}

namespace Blog
{
// Before the network starts, with the other routes of the router
void RegisterApiPackets(Rooftree::Server::WebSocketRouter& router);
}

#endif // BLOG_API_PACKETS_H
