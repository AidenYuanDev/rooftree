/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * WebSocket connections feeding SessionMgr, the way WorldHttpService does for HTTP. Messages are JSON
 * objects with a "type" member, the rest is the body:
 *
 *   {"type": "chat", "text": "hi"}
 *
 * The network thread parses them and looks the type up in a WebSocketRouter (one per server); a
 * known type becomes a WebSocketJsonMessage on the session of the connection's key, given when the
 * connection is made (from the client address, or a login ticket of the handshake). Its handler runs
 * on the main thread and may Reply() from any thread.
 *
 * Rooftree: a WorldWebSocket is a WebSocket::Connection, it has no socket and port of its own.
 * WorldHttpService::WebSocketRoute makes one for a handshake on the HTTP port (plain or TLS) and the
 * HTTP socket runs it from then on.
 *
 * Unknown types are answered with {"type": "error", "code": "unknown_type"}; anything that isn't a
 * JSON object with a string type closes the connection (1007), as no well-behaved client sends that.
 *
 * Binary messages are packets (Packet.h): an opcode, a request id and a FlatBuffer. AddPacket routes
 * an opcode with the verifier of its root type; a packet that is too short or doesn't verify closes the
 * connection (1007), an unknown opcode is answered with an Error (unknown_opcode). Its message type,
 * named by the route, is shared with any front end naming the same: a packet and the HTTP route doing
 * the same thing count against one AntiDOS limit.
 */

#ifndef ROOFTREE_WORLD_WEB_SOCKET_H
#define ROOFTREE_WORLD_WEB_SOCKET_H

#include "Packet.h"
#include "ServerMessage.h"
#include "WebSocketConnection.h"
#include <boost/asio/ip/address.hpp>
#include <boost/json/object.hpp>
#include <flatbuffers/flatbuffers.h>
#include <functional>
#include <map>
#include <span>
#include <unordered_map>

namespace Rooftree::Server
{
class SessionMgr;
class WebSocketJsonMessage;
class WebSocketPacketMessage;
class WorldWebSocket;

using WebSocketHandler = std::function<void(Session& session, WebSocketJsonMessage& message)>;
using PacketHandler = std::function<void(Session& session, WebSocketPacketMessage& message)>;
// Network thread: whether the payload is a valid buffer of the opcode's root type
using PacketVerifier = std::function<bool(std::span<uint8 const> payload)>;

class WebSocketRouter
{
public:
    explicit WebSocketRouter(SessionMgr& sessionMgr) : _sessionMgr(sessionMgr) { }

    SessionMgr& GetSessionMgr() const { return _sessionMgr; }

    // Before the routes: the session class of their message types (SessionMgr), "" the default one. A
    // game's router has its own, "game"
    void SetSessionClass(std::string sessionClass) { _sessionClass = std::move(sessionClass); }

    // Before the network starts. The message type is registered as "ws.<type>"
    void Add(std::string_view type, WebSocketHandler handler);

    template<std::derived_from<Session> SessionImpl>
    void Add(std::string_view type, void (SessionImpl::*handler)(WebSocketJsonMessage&))
    {
        Add(type, [handler](Session& session, WebSocketJsonMessage& message)
        {
            (static_cast<SessionImpl&>(session).*handler)(message);
        });
    }

    // Before the network starts: packets of opcode (not ErrorOpcode), whose message type is messageType
    void AddPacket(uint16 opcode, std::string_view messageType, PacketVerifier verify, PacketHandler handler);

    // A packet whose payload is a Root (a table of generated code), verified as one
    template<typename Root, std::derived_from<Session> SessionImpl>
    void AddPacket(uint16 opcode, std::string_view messageType, void (SessionImpl::*handler)(WebSocketPacketMessage&, Root const&));

    struct Route
    {
        MessageType const* Type;
        WebSocketHandler Handler;
    };

    struct PacketRoute
    {
        uint16 Opcode;
        MessageType const* Type;
        PacketVerifier Verify;
        PacketHandler Handler;
    };

    // Any thread, after the routes are added
    Route const* Find(std::string_view type) const;
    PacketRoute const* FindPacket(uint16 opcode) const;

private:
    SessionMgr& _sessionMgr;
    std::string _sessionClass;
    std::map<std::string, Route, std::less<>> _routes;
    std::unordered_map<uint16, PacketRoute> _packetRoutes;
};

class WebSocketJsonMessage final : public Message
{
public:
    WebSocketJsonMessage(WebSocketRouter::Route const& route, std::string sessionKey, std::shared_ptr<WorldWebSocket> socket,
        std::string type, boost::json::object body);

    std::string const& GetTypeName() const { return _typeName; }
    boost::json::object const& GetBody() const { return _body; }
    std::shared_ptr<WorldWebSocket> const& GetSocket() const { return _socket; }

    // Any thread: {"type": type, ...body} to the connection the message came from
    void Reply(std::string_view type, boost::json::object body = {}) const;

    void Process(Session& session) override;
    void Reject(RejectReason reason) override;

private:
    WebSocketRouter::Route const& _route;
    std::shared_ptr<WorldWebSocket> _socket;
    std::string _typeName;
    boost::json::object _body;
};

class WebSocketPacketMessage final : public Message
{
public:
    WebSocketPacketMessage(WebSocketRouter::PacketRoute const& route, std::string sessionKey, std::shared_ptr<WorldWebSocket> socket,
        PacketHeader header, std::string packet);

    uint16 GetOpcode() const { return _header.Opcode; }
    uint32 GetRequestId() const { return _header.RequestId; }
    // The FlatBuffer, verified
    std::span<uint8 const> GetPayload() const;
    std::shared_ptr<WorldWebSocket> const& GetSocket() const { return _socket; }

    // Any thread: the answer, same opcode and request id, a finished buffer
    void Reply(std::span<uint8 const> payload) const;
    void Reply(flatbuffers::FlatBufferBuilder const& builder) const;
    // An Error packet (Packet.h)
    void ReplyError(uint16 status, std::string_view code, std::string_view message = {},
        std::vector<std::pair<std::string, std::string>> const& fields = {}) const;

    void Process(Session& session) override;
    void Reject(RejectReason reason) override;

private:
    WebSocketRouter::PacketRoute const& _route;
    std::shared_ptr<WorldWebSocket> _socket;
    PacketHeader _header;
    std::string _packet;    // header and payload, as it came
};

template<typename Root, std::derived_from<Session> SessionImpl>
void WebSocketRouter::AddPacket(uint16 opcode, std::string_view messageType, void (SessionImpl::*handler)(WebSocketPacketMessage&, Root const&))
{
    AddPacket(opcode, messageType, [](std::span<uint8 const> payload)
    {
        flatbuffers::Verifier verifier(payload.data(), payload.size());
        return verifier.VerifyBuffer<Root>(nullptr);
    }, [handler](Session& session, WebSocketPacketMessage& message)
    {
        (static_cast<SessionImpl&>(session).*handler)(message, *flatbuffers::GetRoot<Root>(message.GetPayload().data()));
    });
}

class WorldWebSocket : public Net::WebSocket::Connection, public std::enable_shared_from_this<WorldWebSocket>
{
public:
    // sessionKey: the session its messages run in, "ip:..." like the client's HTTP requests or "user:42"
    WorldWebSocket(WebSocketRouter const& router, boost::asio::ip::address remoteAddress, std::string sessionKey)
        : _router(router), _remoteAddress(std::move(remoteAddress)), _sessionKey(std::move(sessionKey)) { }

    // Any thread
    void SendJson(std::string_view type, boost::json::object body = {});
    // A packet: requestId 0 for one the client didn't ask for
    void SendPacket(PacketHeader header, std::span<uint8 const> payload);

    std::string const& GetSessionKey() const { return _sessionKey; }
    boost::asio::ip::address const& GetRemoteIpAddress() const { return _remoteAddress; }

protected:
    void OnMessage(Net::WebSocket::Opcode opcode, std::string&& payload) final;

private:
    void OnPacket(std::string&& packet);

    WebSocketRouter const& _router;
    boost::asio::ip::address _remoteAddress;
    std::string _sessionKey;
};
}

#endif // ROOFTREE_WORLD_WEB_SOCKET_H
