/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "WorldWebSocket.h"
#include "Errors.h"
#include "Log.h"
#include "SessionMgr.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>

namespace Rooftree::Server
{
void WebSocketRouter::Add(std::string_view type, WebSocketHandler handler)
{
    MessageType const& messageType = _sessionMgr.GetMessageTypes().Register("ws." + std::string(type), _sessionClass);
    bool added = _routes.try_emplace(std::string(type), Route{ .Type = &messageType, .Handler = std::move(handler) }).second;
    ASSERT(added, "WebSocket message type %s added twice", std::string(type).c_str());
}

WebSocketRouter::Route const* WebSocketRouter::Find(std::string_view type) const
{
    auto itr = _routes.find(type);
    return itr != _routes.end() ? &itr->second : nullptr;
}

void WebSocketRouter::AddPacket(uint16 opcode, std::string_view messageType, PacketVerifier verify, PacketHandler handler)
{
    ASSERT(opcode != ErrorOpcode, "Opcode 0 is the Error packet's");
    MessageType const& type = _sessionMgr.GetMessageTypes().Register(messageType, _sessionClass);
    bool added = _packetRoutes.try_emplace(opcode, PacketRoute{ .Opcode = opcode, .Type = &type, .Verify = std::move(verify),
        .Handler = std::move(handler) }).second;
    ASSERT(added, "Packet opcode %u added twice", uint32(opcode));
}

WebSocketRouter::PacketRoute const* WebSocketRouter::FindPacket(uint16 opcode) const
{
    auto itr = _packetRoutes.find(opcode);
    return itr != _packetRoutes.end() ? &itr->second : nullptr;
}

WebSocketJsonMessage::WebSocketJsonMessage(WebSocketRouter::Route const& route, std::string sessionKey, std::shared_ptr<WorldWebSocket> socket,
    std::string type, boost::json::object body)
    : Message(*route.Type, std::move(sessionKey), socket->GetRemoteIpAddress()), _route(route), _socket(std::move(socket)),
    _typeName(std::move(type)), _body(std::move(body))
{
}

void WebSocketJsonMessage::Reply(std::string_view type, boost::json::object body) const
{
    _socket->SendJson(type, std::move(body));
}

void WebSocketJsonMessage::Process(Session& session)
{
    _route.Handler(session, *this);
}

void WebSocketJsonMessage::Reject(RejectReason reason)
{
    if (reason == RejectReason::Blocked)
    {
        _socket->Close(Net::WebSocket::CloseCode::PolicyViolation);
        return;
    }

    std::string_view code = reason == RejectReason::RateLimited ? "rate_limited"
        : reason == RejectReason::Overloaded ? "overloaded" : "unavailable";
    Reply("error", { { "code", code }, { "for", _typeName } });
}

WebSocketPacketMessage::WebSocketPacketMessage(WebSocketRouter::PacketRoute const& route, std::string sessionKey,
    std::shared_ptr<WorldWebSocket> socket, PacketHeader header, std::string packet)
    : Message(*route.Type, std::move(sessionKey), socket->GetRemoteIpAddress()), _route(route), _socket(std::move(socket)),
    _header(header), _packet(std::move(packet))
{
}

std::span<uint8 const> WebSocketPacketMessage::GetPayload() const
{
    return { reinterpret_cast<uint8 const*>(_packet.data()) + PacketHeaderSize, _packet.size() - PacketHeaderSize };
}

void WebSocketPacketMessage::Reply(std::span<uint8 const> payload) const
{
    _socket->SendPacket(_header, payload);
}

void WebSocketPacketMessage::Reply(flatbuffers::FlatBufferBuilder const& builder) const
{
    Reply({ builder.GetBufferPointer(), builder.GetSize() });
}

void WebSocketPacketMessage::ReplyError(uint16 status, std::string_view code, std::string_view message,
    std::vector<std::pair<std::string, std::string>> const& fields) const
{
    _socket->SendBinary(WriteErrorPacket(_header.RequestId, _header.Opcode, status, code, message, fields));
}

void WebSocketPacketMessage::Process(Session& session)
{
    _route.Handler(session, *this);
}

void WebSocketPacketMessage::Reject(RejectReason reason)
{
    if (reason == RejectReason::Blocked)
    {
        _socket->Close(Net::WebSocket::CloseCode::PolicyViolation);
        return;
    }

    if (reason == RejectReason::RateLimited)
        ReplyError(429, "rate_limited");
    else
        ReplyError(503, reason == RejectReason::Overloaded ? "overloaded" : "unavailable");
}

void WorldWebSocket::SendPacket(PacketHeader header, std::span<uint8 const> payload)
{
    SendBinary(WritePacket(header, payload));
}

void WorldWebSocket::SendJson(std::string_view type, boost::json::object body)
{
    // "type" first, it is what a reader of the traffic looks for
    boost::json::object message{ { "type", type } };
    for (auto& [key, value] : body)
        if (key != "type")
            message[key] = std::move(value);

    SendText(boost::json::serialize(message));
}

void WorldWebSocket::OnMessage(Net::WebSocket::Opcode opcode, std::string&& payload)
{
    if (opcode == Net::WebSocket::Opcode::Binary)
        return OnPacket(std::move(payload));

    boost::system::error_code error;
    boost::json::value value = boost::json::parse(payload, error);
    boost::json::object* object = error ? nullptr : value.if_object();
    boost::json::value const* typeValue = object ? object->if_contains("type") : nullptr;
    if (!typeValue || !typeValue->is_string())
    {
        RT_LOG_DEBUG("server.websocket", "{} sent a message that is not a JSON object with a type", GetRemoteIpAddress().to_string());
        Close(Net::WebSocket::CloseCode::InvalidPayload, "expected a JSON object with a type");
        return;
    }

    std::string type(typeValue->get_string());
    WebSocketRouter::Route const* route = _router.Find(type);
    if (!route)
    {
        SendJson("error", { { "code", "unknown_type" }, { "for", type } });
        return;
    }

    object->erase("type");
    _router.GetSessionMgr().Enqueue(std::make_unique<WebSocketJsonMessage>(*route, _sessionKey, shared_from_this(), std::move(type),
        std::move(*object)));
}

void WorldWebSocket::OnPacket(std::string&& packet)
{
    Optional<PacketHeader> header = ReadPacketHeader(packet);
    if (!header)
    {
        RT_LOG_DEBUG("server.websocket", "{} sent a packet shorter than its header", GetRemoteIpAddress().to_string());
        Close(Net::WebSocket::CloseCode::InvalidPayload, "packet without a header");
        return;
    }

    WebSocketRouter::PacketRoute const* route = _router.FindPacket(header->Opcode);
    if (!route)
    {
        SendBinary(WriteErrorPacket(header->RequestId, header->Opcode, 400, "unknown_opcode"));
        return;
    }

    std::span<uint8 const> payload(reinterpret_cast<uint8 const*>(packet.data()) + PacketHeaderSize, packet.size() - PacketHeaderSize);
    if (!route->Verify(payload))
    {
        RT_LOG_DEBUG("server.websocket", "{} sent a packet of opcode {} that doesn't verify", GetRemoteIpAddress().to_string(), header->Opcode);
        Close(Net::WebSocket::CloseCode::InvalidPayload, "packet doesn't verify");
        return;
    }

    _router.GetSessionMgr().Enqueue(std::make_unique<WebSocketPacketMessage>(*route, _sessionKey, shared_from_this(), *header, std::move(packet)));
}
}
