/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ArenaSocket.h"
#include "ArenaPackets.h"
#include "ArenaSession.h"
#include "Log.h"
#include "World.h"

namespace
{
// Client messages are small: inputs, chat lines, pings
constexpr std::size_t MaxClientMessageSize = 4 * 1024;

// name of "/arena?ticket=...&x=y", empty if missing. Tickets are hex, the formats words, so no
// percent-decoding needed
std::string_view GetQueryParameter(std::string_view target, std::string_view name)
{
    std::size_t query = target.find('?');
    if (query == std::string_view::npos)
        return {};

    std::string_view params = target.substr(query + 1);
    while (!params.empty())
    {
        std::size_t end = params.find('&');
        std::string_view param = params.substr(0, end);
        if (param.size() > name.size() && param.starts_with(name) && param[name.size()] == '=')
            return param.substr(name.size() + 1);

        if (end == std::string_view::npos)
            break;
        params.remove_prefix(end + 1);
    }

    return {};
}
}

ArenaSocketMgr& ArenaSocketMgr::Instance()
{
    static ArenaSocketMgr instance;
    return instance;
}

void ArenaSocket::Start()
{
    SetMaxMessageSize(MaxClientMessageSize);
    BaseSocket::Start();
}

bool ArenaSocket::OnHandshake(Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response)
{
    std::string_view target = Rooftree::Net::Http::ToStdStringView(request.target());
    if (!target.starts_with("/arena"))
    {
        response.result(boost::beast::http::status::not_found);
        return false;
    }

    std::string_view format = GetQueryParameter(target, "format");
    if (!format.empty() && format != "json" && format != "packets")
    {
        response.result(boost::beast::http::status::bad_request);
        return false;
    }
    _packets = format == "packets";

    _ticket = sTicketMgr->Consume(GetQueryParameter(target, "ticket"));
    if (!_ticket)
    {
        RT_LOG_DEBUG("arena.session", "{} refused: unknown or expired ticket", GetClientInfo());
        response.result(boost::beast::http::status::unauthorized);
        return false;
    }

    return true;
}

void ArenaSocket::OnOpen()
{
    std::shared_ptr<ArenaSession> session = std::make_shared<ArenaSession>(_ticket->AccountId, _ticket->Name,
        std::static_pointer_cast<ArenaSocket>(shared_from_this()));
    _session = session;

    RT_LOG_INFO("arena.session", "{} logged in as {} (account {})", GetClientInfo(), _ticket->Name, _ticket->AccountId);
    sWorld->AddSession(std::move(session));
}

void ArenaSocket::Write(ServerPacket const& packet, bool /*unreliable*/)
{
    if (_packets)
        SendBinary(packet.GetPacket());
    else
        SendText(packet.GetJson());
}

void ArenaSocket::OnMessage(Rooftree::Net::WebSocket::Opcode opcode, std::string&& payload)
{
    // Read and verified here so the world thread only gets valid messages
    bool text = opcode == Rooftree::Net::WebSocket::Opcode::Text;
    std::unique_ptr<ClientMessage> message;
    switch (text ? ReadClientJson(payload, message) : ReadClientPacket(payload, message))
    {
        case ClientMessageResult::Ok:
            break;
        case ClientMessageResult::Unknown:
            RT_LOG_DEBUG("arena.session", "{} sent a message of a type we don't know, ignored", GetClientInfo());
            return;
        case ClientMessageResult::Invalid:
            RT_LOG_DEBUG("arena.session", "{} sent {}", GetClientInfo(), text ? "a message that is not a JSON object with a type" : "an invalid packet");
            Close(Rooftree::Net::WebSocket::CloseCode::InvalidPayload, text ? "expected a JSON object with a type" : "invalid packet");
            return;
    }

    std::shared_ptr<ArenaSession> session = _session.lock();
    if (!session)
        return;

    if (!session->QueueMessage(std::move(message)))
    {
        RT_LOG_WARN("arena.session", "{} ({}) is flooding, disconnecting", GetClientInfo(), session->GetName());
        Close(Rooftree::Net::WebSocket::CloseCode::PolicyViolation, "too many messages");
    }
}
