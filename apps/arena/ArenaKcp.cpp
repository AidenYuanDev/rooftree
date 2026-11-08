/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ArenaKcp.h"
#include "ArenaPackets.h"
#include "ArenaSession.h"
#include "Log.h"
#include "World.h"

namespace
{
// As ArenaSocket's: inputs, chat lines, pings
constexpr std::size_t MaxClientMessageSize = 4 * 1024;
}

ArenaKcpServer& ArenaKcpServer::Instance()
{
    static ArenaKcpServer instance;
    return instance;
}

std::shared_ptr<Rooftree::Net::Kcp::Connection> ArenaKcpServer::OnHello(std::string_view token, boost::asio::ip::address const& /*address*/,
    std::function<bool(Rooftree::Net::Kcp::Key const&)> const& verify)
{
    // Used up only by a Hello sealed with its key: someone who saw the ticket alone can't take it
    Optional<LoginTicket> ticket = sTicketMgr->Consume(token, [&](LoginTicket const& candidate) { return verify(candidate.KcpKey); });
    if (!ticket)
        return nullptr;

    return std::make_shared<ArenaKcpConnection>(std::move(*ticket));
}

void ArenaKcpConnection::Write(ServerPacket const& packet, bool unreliable)
{
    if (unreliable)
        SendUnreliable(packet.GetPacket());
    else if (!Send(packet.GetPacket()) && IsOpen())
        RT_LOG_ERROR("arena.session", "{}: a {} of {} bytes is too large for KCP", GetClientInfo(), GetOpcodeName(packet.GetOpcode()),
            packet.GetPacket().size());
}

void ArenaKcpConnection::OnOpen()
{
    std::shared_ptr<ArenaSession> session = std::make_shared<ArenaSession>(_ticket.AccountId, _ticket.Name,
        std::static_pointer_cast<ArenaKcpConnection>(shared_from_this()));
    _session = session;

    RT_LOG_INFO("arena.session", "{} logged in over KCP as {} (account {})", GetClientInfo(), _ticket.Name, _ticket.AccountId);
    sWorld->AddSession(std::move(session));
}

void ArenaKcpConnection::OnMessage(std::string&& payload, bool /*reliable*/)
{
    if (payload.size() > MaxClientMessageSize)
    {
        Close(Rooftree::Net::Kcp::CloseCode::MessageTooBig, "message too big");
        return;
    }

    std::unique_ptr<ClientMessage> message;
    switch (ReadClientPacket(payload, message))
    {
        case ClientMessageResult::Ok:
            break;
        case ClientMessageResult::Unknown:
            RT_LOG_DEBUG("arena.session", "{} sent a packet of an opcode we don't know, ignored", GetClientInfo());
            return;
        case ClientMessageResult::Invalid:
            RT_LOG_DEBUG("arena.session", "{} sent an invalid packet", GetClientInfo());
            Close(uint16(Rooftree::Net::WebSocket::CloseCode::InvalidPayload), "invalid packet");
            return;
    }

    std::shared_ptr<ArenaSession> session = _session.lock();
    if (!session)
        return;

    if (!session->QueueMessage(std::move(message)))
    {
        RT_LOG_WARN("arena.session", "{} ({}) is flooding, disconnecting", GetClientInfo(), session->GetName());
        Close(uint16(Rooftree::Net::WebSocket::CloseCode::PolicyViolation), "too many messages");
    }
}
