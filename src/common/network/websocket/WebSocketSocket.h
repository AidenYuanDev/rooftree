/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A WebSocket server connection on top of Rooftree::Net::Socket, see WebSocketFrame.h
 * for why Beast's websocket::stream is not used.
 *
 *   class ChatSocket final : public WebSocket::BaseSocket<>
 *   {
 *       using BaseSocket::BaseSocket;
 *       void OnMessage(WebSocket::Opcode opcode, std::string&& payload) override { SendText(payload); }
 *   };
 *
 * and a SocketMgr<Traits> with SocketType = ChatSocket, exactly like a plain TCP server.
 *
 * Threads: ReadHandler, OnHandshake, OnOpen and OnMessage run on the socket's NetworkThread.
 * SendText/SendBinary/Close may be called from any thread (a world thread): like WorldSocket::SendPacket
 * they only push to a lock-free queue, Update() moves it into the write queue on the network thread.
 *
 * After the handshake the socket runs its own WebSocket::Connection (WebSocketConnection.h), the same
 * connection an HTTP socket runs after an upgrade on its port. Plain TCP only; an HTTP service with TLS
 * upgrades its own connections (wss://), see WorldHttpService::WebSocketRoute.
 */

#ifndef ROOFTREE_WEBSOCKET_SOCKET_H
#define ROOFTREE_WEBSOCKET_SOCKET_H

#include "BaseHttpSocket.h"
#include "Socket.h"
#include "WebSocketConnection.h"
#include "WebSocketFrame.h"
#include <boost/beast/http/error.hpp>

namespace Rooftree::Net::WebSocket
{
template<class Stream = IoContextTcpSocket>
class BaseSocket : public Rooftree::Net::Socket<Stream>, public Connection, private ConnectionTransport
{
    using Base = Rooftree::Net::Socket<Stream>;

public:
    using Base::Base;

    BaseSocket(BaseSocket const& other) = delete;
    BaseSocket(BaseSocket&& other) = delete;
    BaseSocket& operator=(BaseSocket const& other) = delete;
    BaseSocket& operator=(BaseSocket&& other) = delete;

    ~BaseSocket() = default;

    void Start() override
    {
        _handshakeParser.emplace();
        _handshakeParser->header_limit(HandshakeHeaderLimit);
        _handshakeParser->body_limit(0);
        _handshakeParser->eager(true);
        this->StartReading();
    }

    bool Update() override
    {
        if (_isOpen)
            Flush(*this);

        if (Base::Update())
            return true;

        Closed();
        return false;
    }

    SocketReadCallbackResult ReadHandler() final
    {
        MessageBuffer& packet = this->GetReadBuffer();
        if (!_isOpen)
        {
            if (!ReadHandshake(packet))
                return SocketReadCallbackResult::Stop;

            if (!_isOpen)
                return SocketReadCallbackResult::KeepReading;
        }

        return Read(*this, packet);
    }

    // Before Start(), 0 disables either. handshake: from the connection to the end of the opening
    // handshake; idle: without anything from the client once open (a page's heartbeat, a ping)
    void SetTimeouts(Milliseconds handshake, Milliseconds idle)
    {
        _idleTimeout = idle;
        this->SetDeadline(handshake);
    }

    std::string GetClientInfo() const { return StringFormat("[{}:{}]", this->GetRemoteIpAddress(), this->GetRemotePort()); }

protected:
    // The opening handshake passed the protocol checks. Return false to refuse the connection, after
    // setting response's status (403 if left at 101) and body. The request target carries the query
    // string, e.g. /ws?ticket=..., that is where browsers can pass credentials
    virtual bool OnHandshake(Http::Request const& /*request*/, Http::Response& /*response*/) { return true; }

private:
    static constexpr uint32 HandshakeHeaderLimit = 8 * 1024;

    // ConnectionTransport
    void WriteFrame(MessageBuffer&& frame) override { this->QueuePacket(std::move(frame)); }
    void CloseAfterWrites() override { this->DelayedCloseSocket(); }
    bool IsTransportOpen() const override { return this->IsOpen(); }
    std::string GetPeerInfo() const override { return GetClientInfo(); }

    // false if the connection is being closed
    bool ReadHandshake(MessageBuffer& packet)
    {
        boost::system::error_code ec;
        std::size_t used = _handshakeParser->put(boost::asio::const_buffer(packet.GetReadPointer(), packet.GetActiveSize()), ec);
        packet.ReadCompleted(used);

        if (ec == boost::beast::http::error::need_more)
            return true;

        if (ec)
        {
            RT_LOG_DEBUG("network.websocket", "{} invalid handshake request: {}", GetClientInfo(), ec.message());
            this->CloseSocket();
            return false;
        }

        if (!_handshakeParser->is_done())
            return true;

        Http::Request request = _handshakeParser->release();
        _handshakeParser.reset();

        Http::Response response;
        bool accepted = AcceptHandshake(request, response);
        if (accepted && !OnHandshake(request, response))
        {
            accepted = false;
            if (response.result() == boost::beast::http::status::switching_protocols)
                response.result(boost::beast::http::status::forbidden);
            response.erase(boost::beast::http::field::upgrade);
            response.erase(boost::beast::http::field::sec_websocket_accept);
        }

        if (!accepted)
        {
            RT_LOG_DEBUG("network.websocket", "{} handshake refused with {}", GetClientInfo(), response.result_int());
            response.set(boost::beast::http::field::connection, "close");
            response.prepare_payload();
            this->QueuePacket(Http::SerializeResponse(request, response));
            this->DelayedCloseSocket();
            return false;
        }

        this->QueuePacket(Http::SerializeResponse(request, response));
        _isOpen = true;
        this->ClearDeadline();
        this->SetIdleTimeout(_idleTimeout);

        RT_LOG_DEBUG("network.websocket", "{} connected to {}", GetClientInfo(), Http::ToStdStringView(request.target()));
        Open(*this);
        return true;
    }

    Optional<Http::RequestParser> _handshakeParser;
    bool _isOpen = false;       // handshake done
    Milliseconds _idleTimeout = 0ms;
};
}

#endif // ROOFTREE_WEBSOCKET_SOCKET_H
