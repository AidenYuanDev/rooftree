/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The game over UDP, for native clients (a browser can only open the WebSocket): what ArenaSocket
 * does for a WebSocket, for a Kcp::Connection. The login answered a ticket and a key; the Hello
 * carries the ticket, sealed with the key, and opens the same ArenaSession the WebSocket would. Every
 * message is a packet (ArenaPackets.h); the state each tick goes unreliably, a lost one is replaced by
 * the next.
 */

#ifndef ARENA_KCP_H
#define ARENA_KCP_H

#include "ArenaConnection.h"
#include "KcpServer.h"
#include "TicketMgr.h"

class ArenaSession;

class ArenaKcpConnection final : public Rooftree::Net::Kcp::Connection, public ArenaConnection
{
public:
    explicit ArenaKcpConnection(LoginTicket ticket) : _ticket(std::move(ticket)) { }

    void Write(ServerPacket const& packet, bool unreliable) override;
    void Disconnect(Rooftree::Net::WebSocket::CloseCode code, std::string_view reason) override { Close(uint16(code), reason); }
    bool IsConnectionOpen() const override { return IsOpen(); }
    std::string GetConnectionInfo() const override { return GetClientInfo(); }

protected:
    void OnOpen() override;
    void OnMessage(std::string&& message, bool reliable) override;

private:
    LoginTicket _ticket;
    std::weak_ptr<ArenaSession> _session;
};

class ArenaKcpServer final : public Rooftree::Net::Kcp::Server
{
public:
    static ArenaKcpServer& Instance();

protected:
    std::shared_ptr<Rooftree::Net::Kcp::Connection> OnHello(std::string_view token, boost::asio::ip::address const& address,
        std::function<bool(Rooftree::Net::Kcp::Key const&)> const& verify) override;
};

#define sArenaKcpServer ArenaKcpServer::Instance()

#endif // ARENA_KCP_H
