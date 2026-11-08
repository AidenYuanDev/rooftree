/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "KcpClient.h"
#include "CryptoRandom.h"
#include "Errors.h"
#include "IpAddress.h"
#include "Log.h"
#include "Task.h"
#include "Timer.h"
#include <boost/asio/post.hpp>

namespace Rooftree::Net::Kcp
{
namespace
{
std::span<uint8 const> AsBytes(std::string_view text)
{
    return { reinterpret_cast<uint8 const*>(text.data()), text.size() };
}
}

Client::Client(Asio::IoContext& ioContext, Settings const& settings /*= {}*/) : _ioContext(ioContext), _settings(settings),
    _socket(ioContext), _updateTimer(ioContext)
{
}

Client::~Client() = default;

void Client::Connect(Endpoint const& server, std::string token, Key const& key)
{
    ASSERT(token.size() && token.size() <= MaxTokenSize);

    boost::asio::post(_ioContext, [self = shared_from_this(), server, token = std::move(token), key]() mutable
    {
        if (self->_state != State::Idle)
            return;

        self->_token = std::move(token);
        self->_key = key;
        self->_state = State::Connecting;
        self->_connectStart = getMSTime();
        self->_helloBody[0] = ProtocolVersion;
        Crypto::GetRandomBytes(self->_helloBody.data() + 1, self->_helloBody.size() - 1);

        // Another client may be given this key too (a token tried again): not the same counters, so not
        // the same nonces. 63 bits, it never wraps
        Crypto::GetRandomBytes(reinterpret_cast<uint8*>(&self->_helloCounter), sizeof(self->_helloCounter));
        self->_helloCounter >>= 1;

        // Connected: the kernel drops datagrams from anyone but the server
        boost::system::error_code error;
        self->_socket.open(server.protocol(), error);
        if (!error)
            self->_socket.connect(server, error);
        if (!error)
            self->_socket.non_blocking(true, error);
        if (error)
        {
            RT_LOG_DEBUG("network.kcp", "Kcp::Client: can't connect to [{}:{}]: {}", server.address(), server.port(), error.message());
            self->Finish(CloseCode::GoingAway, error.message(), false);
            return;
        }

        self->SendHello();
        ReceiveLoop(self);
        UpdateLoop(self);
    });
}

bool Client::Send(std::string_view message)
{
    if (!_open || message.size() > std::min(_settings.MaxMessageSize, _settings.GetMaxMessageSizeLimit()))
        return false;

    boost::asio::post(_ioContext, [self = shared_from_this(), message = std::string(message)]
    {
        if (self->_state != State::Open)
            return;

        self->_link->SendReliable(AsBytes(message));
        self->_link->Flush();
    });
    return true;
}

void Client::SendUnreliable(std::string_view message)
{
    if (message.size() > MaxBodySize)
    {
        Send(message);
        return;
    }

    if (!_open)
        return;

    boost::asio::post(_ioContext, [self = shared_from_this(), message = std::string(message)]
    {
        if (self->_state == State::Open)
            self->_link->Send(DatagramType::Unreliable, AsBytes(message));
    });
}

void Client::Close(uint16 code /*= CloseCode::Normal*/, std::string_view reason /*= {}*/)
{
    boost::asio::post(_ioContext, [self = shared_from_this(), code, reason = std::string(reason.substr(0, MaxCloseReasonSize))]
    {
        switch (self->_state)
        {
            case State::Connecting:
                self->Finish(code, reason, false);
                break;
            case State::Open:
                self->_state = State::Closing;
                self->_closeCode = code;
                self->_closeReason = reason;
                self->_closeDeadline = getMSTime() + uint32(self->_settings.CloseTimeout.count());
                break;
            default:
                break;
        }
    });
}

void Client::SendHello()
{
    std::vector<uint8> clear(1 + _token.size());
    clear[0] = uint8(_token.size());
    std::copy(_token.begin(), _token.end(), clear.begin() + 1);

    Cipher(*_key).Seal(Direction::ClientToServer, { .Conv = 0, .Type = DatagramType::Hello, .Counter = _helloCounter++ }, clear, _helloBody, _datagram);
    Output(_datagram);
    _lastHello = getMSTime();
}

void Client::Output(std::span<uint8 const> datagram)
{
    boost::system::error_code error;
    _socket.send(boost::asio::buffer(datagram.data(), datagram.size()), 0, error);
    _lastSend = getMSTime();
}

Rooftree::Detached Client::ReceiveLoop(std::shared_ptr<Client> self)
{
    while (self->_state != State::Closed)
    {
        auto [error, size] = co_await Rooftree::Await<boost::system::error_code, std::size_t>([&](auto done)
        {
            self->_socket.async_receive(boost::asio::buffer(self->_receiveBuffer), std::move(done));
        });

        if (self->_state == State::Closed || error == boost::asio::error::operation_aborted)
            break;

        // Errors of a connected UDP socket are ICMP errors (port unreachable while the server restarts):
        // nothing to do but keep listening, the timeouts decide when to give up
        if (!error)
            self->HandleDatagram({ self->_receiveBuffer.data(), size });
    }
}

void Client::HandleDatagram(std::span<uint8 const> datagram)
{
    if (datagram.size() < HeaderSize + TagSize || datagram.size() > MaxDatagramSize)
        return;

    Header header = ReadHeader(datagram);
    if (!_link)
    {
        HandleWelcome(datagram, header);
        return;
    }

    if (header.Conv != _link->GetConv() || !_link->Open(datagram, header, 0, _body))
        return;

    _lastReceive = getMSTime();
    switch (header.Type)
    {
        case DatagramType::Data:
        {
            if (!_link->Input(_body))
            {
                Finish(CloseCode::ProtocolError, "invalid KCP data", true);
                return;
            }

            std::string message;
            while (_state == State::Open || _state == State::Closing)
            {
                Link::ReceiveResult result = _link->Receive(message);
                if (result == Link::ReceiveResult::None)
                    break;
                if (result == Link::ReceiveResult::TooBig)
                {
                    Finish(CloseCode::MessageTooBig, "message too big", true);
                    return;
                }
                if (_state == State::Open)
                    OnMessage(std::move(message), true);
            }

            // The acknowledgments, after the messages were taken: they tell the server our receive
            // window, and with the messages still queued (a lost segment arrived, everything behind it
            // completed) it could be 0. The server then waits for KCP's window probe, 7 s
            if (_state != State::Closed)
                _link->Flush();
            break;
        }
        case DatagramType::Unreliable:
            if (_state == State::Open)
                OnMessage(std::string(_body.begin(), _body.end()), false);
            break;
        case DatagramType::Pong:
            if (_body.size() == 4)
            {
                uint32 sent = uint32(_body[0]) | uint32(_body[1]) << 8 | uint32(_body[2]) << 16 | uint32(_body[3]) << 24;
                _roundTripTime = getMSTime() - sent;
            }
            break;
        case DatagramType::Close:
        {
            uint16 code = CloseCode::ProtocolError;
            std::string reason;
            if (!ReadCloseBody(_body, code, reason))
                reason = "invalid close";
            Finish(code, reason, false);
            break;
        }
        default:    // a repeated Welcome
            break;
    }
}

void Client::HandleWelcome(std::span<uint8 const> datagram, Header const& header)
{
    // Before the Link only the Welcome, or the Close of a server that refused our version
    if (header.Type != DatagramType::Welcome && !(header.Type == DatagramType::Close && !header.Conv))
        return;

    if (!Cipher(*_key).Open(Direction::ServerToClient, datagram, 0, _body))
        return;

    if (header.Type == DatagramType::Close)
    {
        uint16 code = CloseCode::ProtocolError;
        std::string reason;
        ReadCloseBody(_body, code, reason);
        Finish(code, reason, false);
        return;
    }

    if (!header.Conv || _body.size() != 1 || _body[0] != ProtocolVersion)
        return;

    _link = std::make_unique<Link>(header.Conv, *_key, Direction::ClientToServer, _settings, [this](std::span<uint8 const> out) { Output(out); }, _helloCounter);
    _link->MarkSeen(header.Counter);
    _conv = header.Conv;
    _lastReceive = getMSTime();
    _nextUpdate = _lastReceive;
    _state = State::Open;
    _open = true;
    OnOpen();
}

Rooftree::Detached Client::UpdateLoop(std::shared_ptr<Client> self)
{
    while (self->_state != State::Closed)
    {
        self->_updateTimer.expires_after(Milliseconds(std::max(self->_settings.Interval, 1)));
        auto [error] = co_await Rooftree::Await<boost::system::error_code>([&](auto done) { self->_updateTimer.async_wait(std::move(done)); });
        if (error || self->_state == State::Closed)
            break;

        self->Update();
    }
}

void Client::Update()
{
    uint32 now = getMSTime();
    if (_state == State::Connecting)
    {
        if (now - _connectStart >= uint32(_settings.HandshakeTimeout.count()))
        {
            Finish(CloseCode::Timeout, "no answer from the server", false);
            return;
        }

        if (now - _lastHello >= uint32(_settings.HelloInterval.count()))
            SendHello();
        return;
    }

    if (int32(now - _nextUpdate) >= 0)
    {
        _link->Update(now);
        _nextUpdate = _link->Check(now);
    }

    if (now - _lastSend >= uint32(_settings.PingInterval.count()))
    {
        uint8 const body[] = { uint8(now), uint8(now >> 8), uint8(now >> 16), uint8(now >> 24) };
        _link->Send(DatagramType::Ping, body);
    }

    if (_link->IsDead())
        Finish(CloseCode::DeadLink, "no acknowledgments", true);
    else if (_link->GetWaitingSegments() > _settings.MaxWaitingSegments)
        Finish(CloseCode::SendBufferFull, "send buffer full", true);
    else if (now - _lastReceive > uint32(_settings.IdleTimeout.count()))
        Finish(CloseCode::Timeout, "timed out", true);
    else if (_state == State::Closing && (!_link->GetWaitingSegments() || int32(now - _closeDeadline) >= 0))
        Finish(_closeCode, _closeReason, true);
}

void Client::Finish(uint16 code, std::string_view reason, bool sendClose)
{
    if (_state == State::Closed)
        return;

    if (sendClose && _link)
        _link->Send(DatagramType::Close, MakeCloseBody(code, reason));

    _state = State::Closed;
    _open = false;

    boost::system::error_code ignored;
    _socket.close(ignored);
    _updateTimer.cancel();

    // reason may be _closeReason, which the callback is free to look at
    std::string reasonCopy(reason);
    OnClose(code, reasonCopy);
}
}
