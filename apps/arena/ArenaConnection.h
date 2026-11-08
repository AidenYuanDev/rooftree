/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What an ArenaSession sends through, whichever transport the client came by: the WebSocket of the
 * browser page (ArenaSocket) or KCP over UDP for a native client (ArenaKcp). The session and World
 * send ServerPackets; the connection sends the packet or, to the page, its JSON (ArenaPackets.h).
 */

#ifndef ARENA_CONNECTION_H
#define ARENA_CONNECTION_H

#include "WebSocketFrame.h"
#include <string>
#include <string_view>

class ServerPacket;

class ArenaConnection
{
public:
    virtual ~ArenaConnection() = default;

    // World thread. unreliable: may be lost, a newer one replaces it (the state); a WebSocket sends it anyway
    virtual void Write(ServerPacket const& packet, bool unreliable) = 0;
    // Any thread, with a WebSocket close code (KCP numbers them the same way)
    virtual void Disconnect(Rooftree::Net::WebSocket::CloseCode code, std::string_view reason) = 0;
    // Any thread: false once closed, by either side
    virtual bool IsConnectionOpen() const = 0;
    // For logs
    virtual std::string GetConnectionInfo() const = 0;
};

#endif // ARENA_CONNECTION_H
