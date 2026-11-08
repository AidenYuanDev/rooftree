/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The game connection: checks the login ticket during the WebSocket handshake,
 * creates the ArenaSession and hands it to World, then turns every message into a ClientMessage for
 * the session: a text message is the page's JSON, a binary one a packet. It answers in JSON, or in
 * packets if the handshake asked for format=packets (a native client that can't use UDP). Runs on its NetworkThread; the session owns the socket, the socket only has a weak_ptr
 * back, so a session World dropped stops receiving at once.
 */

#ifndef ARENA_SOCKET_H
#define ARENA_SOCKET_H

#include "ArenaConnection.h"
#include "NetworkThread.h"
#include "SocketMgr.h"
#include "TicketMgr.h"
#include "WebSocketSocket.h"

class ArenaSession;

class ArenaSocket final : public Rooftree::Net::WebSocket::BaseSocket<>, public ArenaConnection
{
public:
    using BaseSocket::BaseSocket;

    void Start() override;

    void Write(ServerPacket const& packet, bool unreliable) override;
    void Disconnect(Rooftree::Net::WebSocket::CloseCode code, std::string_view reason) override { BaseSocket::Close(code, reason); }
    bool IsConnectionOpen() const override { return IsOpen(); }
    std::string GetConnectionInfo() const override { return GetClientInfo(); }

protected:
    bool OnHandshake(Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response) override;
    void OnOpen() override;
    void OnMessage(Rooftree::Net::WebSocket::Opcode opcode, std::string&& payload) override;

private:
    Optional<LoginTicket> _ticket;
    bool _packets = false;
    std::weak_ptr<ArenaSession> _session;
};

class ArenaNetworkThread final : public Rooftree::Net::NetworkThread<ArenaSocket, ArenaNetworkThread> { };

class ArenaSocketMgr;

struct ArenaSocketMgrTraits
{
    using Self = ArenaSocketMgr;
    using SocketType = ArenaSocket;
    using ThreadType = ArenaNetworkThread;
};

class ArenaSocketMgr final : public Rooftree::Net::SocketMgr<ArenaSocketMgrTraits>
{
public:
    static ArenaSocketMgr& Instance();
};

#define sArenaSocketMgr ArenaSocketMgr::Instance()

#endif // ARENA_SOCKET_H
