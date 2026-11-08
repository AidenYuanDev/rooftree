/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "KcpServer.h"
#include "Errors.h"
#include "IpAddress.h"
#include "Log.h"
#include "Task.h"
#include "Optional.h"
#include "Random.h"
#include "StringFormat.h"
#include "Timer.h"
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <future>

namespace Rooftree::Net::Kcp
{
namespace
{
std::span<uint8 const> AsBytes(std::string_view text)
{
    return { reinterpret_cast<uint8 const*>(text.data()), text.size() };
}

// Socket buffers for bursts: a world update sends every client its state at once
constexpr int SocketBufferSize = 4 * 1024 * 1024;
}

Connection::~Connection() = default;

bool Connection::Send(std::string_view message)
{
    if (!_open || _closeQueued || message.size() > _maxMessageSize)
        return false;

    Enqueue(new Outgoing{ .Type = Outgoing::Reliable, .Data = std::string(message) });
    return true;
}

void Connection::SendUnreliable(std::string_view message)
{
    if (message.size() > MaxBodySize)
    {
        Send(message);
        return;
    }

    if (!_open || _closeQueued)
        return;

    Enqueue(new Outgoing{ .Type = Outgoing::Unreliable, .Data = std::string(message) });
}

void Connection::Close(uint16 code /*= CloseCode::Normal*/, std::string_view reason /*= {}*/)
{
    if (!_open || _closeQueued.exchange(true))
        return;

    Enqueue(new Outgoing{ .Type = Outgoing::CloseAfter, .Code = code, .Data = std::string(reason.substr(0, MaxCloseReasonSize)) });
}

void Connection::Enqueue(Outgoing* outgoing)
{
    _outgoing.Enqueue(outgoing);
    _hasOutgoing = true;
}

std::string Connection::GetClientInfo() const
{
    return StringFormat("[{}:{}]", _helloEndpoint.address(), _helloEndpoint.port());
}

void Connection::Init(Server& server, uint32 conv, Key const& key, IoContextUdpSocket::endpoint_type const& endpoint,
    std::string token, std::span<uint8 const> helloBody, std::shared_ptr<void> connectionTicket, uint64 helloCounter)
{
    _server = &server;
    _conv = conv;
    _remoteAddress = endpoint.address();
    _helloEndpoint = endpoint;
    _endpoint = endpoint;
    _token = std::move(token);
    _helloBody.assign(helloBody.begin(), helloBody.end());
    _maxMessageSize = std::min(server._settings.MaxMessageSize, server._settings.GetMaxMessageSizeLimit());
    _connectionTicket = std::move(connectionTicket);
    _link = std::make_unique<Link>(conv, key, Direction::ServerToClient, server._settings, [this](std::span<uint8 const> datagram)
    {
        _server->SendTo(datagram, _endpoint);
    });
    _link->MarkSeen(helloCounter);
    _lastReceive = getMSTime();
    _nextUpdate = _lastReceive;
}

void Connection::SendWelcome()
{
    uint8 const body[] = { ProtocolVersion };
    _link->Send(DatagramType::Welcome, body);
}

void Connection::HandleHello(std::span<uint8 const> datagram, Header const& header, std::size_t clearSize,
    IoContextUdpSocket::endpoint_type const& from, uint32 now)
{
    // The Welcome was lost (or is on its way): the same again
    if (_state == State::Closed || !_link->Open(datagram, header, clearSize, _body) || _body != _helloBody)
    {
        ++_server->_dropped;
        return;
    }

    _lastReceive = now;
    _endpoint = from;
    SendWelcome();
}

void Connection::HandleDatagram(std::span<uint8 const> datagram, Header const& header, IoContextUdpSocket::endpoint_type const& from, uint32 now)
{
    if (_state == State::Closed || !_link->Open(datagram, header, 0, _body))
    {
        ++_server->_dropped;
        return;
    }

    _lastReceive = now;

    // Only the newest datagram moves it: an old one replayed from elsewhere doesn't authenticate as new
    if (from != _endpoint && header.Counter == _link->GetHighestSeen())
    {
        RT_LOG_DEBUG("network.kcp", "{} conv {} moved to [{}:{}]", GetClientInfo(), _conv, from.address(), from.port());
        _endpoint = from;
    }

    switch (header.Type)
    {
        case DatagramType::Data:
        {
            if (!_link->Input(_body))
            {
                Finish(CloseCode::ProtocolError, "invalid KCP data", true);
                return;
            }

            // The acknowledgments go out with the next update, within a millisecond, after the messages
            // below were taken: they tell the client our receive window, which would be smaller (or 0,
            // the client then waits for KCP's 7 s window probe) with them still queued
            _needsFlush = true;

            std::string message;
            while (_state != State::Closed)
            {
                switch (_link->Receive(message))
                {
                    case Link::ReceiveResult::None:
                        return;
                    case Link::ReceiveResult::TooBig:
                        Finish(CloseCode::MessageTooBig, "message too big", true);
                        return;
                    case Link::ReceiveResult::Message:
                        Deliver(std::move(message), true);
                        break;
                }
            }
            break;
        }
        case DatagramType::Unreliable:
            Deliver(std::string(_body.begin(), _body.end()), false);
            break;
        case DatagramType::Ping:
            if (_body.size() <= 8)
                _link->Send(DatagramType::Pong, _body);
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
        default:    // a Welcome or a Pong is not for us
            break;
    }
}

void Connection::Deliver(std::string&& message, bool reliable)
{
    // Closing: what arrives after Close() is ignored, as after a WebSocket close frame
    if (_state != State::Open)
        return;

    // A handler that throws loses its connection, not the server and the other connections
    try
    {
        OnMessage(std::move(message), reliable);
    }
    catch (std::exception const& e)
    {
        RT_LOG_ERROR("network.kcp", "{} message handler threw: {}", GetClientInfo(), e.what());
        Finish(CloseCode::InternalError, "internal error", true);
    }
}

bool Connection::Update(uint32 now)
{
    if (_state == State::Closed)
        return false;

    if (_hasOutgoing.exchange(false))
    {
        Outgoing* raw;
        while (_outgoing.Dequeue(raw))
        {
            std::unique_ptr<Outgoing> outgoing(raw);
            if (_state != State::Open)
                continue;

            switch (outgoing->Type)
            {
                case Outgoing::Reliable:
                    // Send() checked the size, KCP refuses nothing else
                    _link->SendReliable(AsBytes(outgoing->Data));
                    _needsFlush = true;
                    break;
                case Outgoing::Unreliable:
                    // Reliable messages queued before go out before it, as they were sent
                    if (_needsFlush)
                    {
                        _link->Flush();
                        _needsFlush = false;
                    }
                    _link->Send(DatagramType::Unreliable, AsBytes(outgoing->Data));
                    break;
                case Outgoing::CloseAfter:
                    _state = State::Closing;
                    _closeCode = outgoing->Code;
                    _closeReason = std::move(outgoing->Data);
                    _closeDeadline = now + uint32(_server->_settings.CloseTimeout.count());
                    break;
            }
        }
    }

    // Sent now instead of at KCP's next interval: a message is on its way within a millisecond
    if (_needsFlush)
    {
        _link->Flush();
        _needsFlush = false;
    }

    if (int32(now - _nextUpdate) >= 0)
    {
        _link->Update(now);
        _nextUpdate = _link->Check(now);
    }

    Settings const& settings = _server->_settings;
    if (_link->IsDead())
        Finish(CloseCode::DeadLink, "no acknowledgments", true);
    else if (_link->GetWaitingSegments() > settings.MaxWaitingSegments)
        Finish(CloseCode::SendBufferFull, "send buffer full", true);
    else if (now - _lastReceive > uint32(settings.IdleTimeout.count()))
        Finish(CloseCode::Timeout, "timed out", true);
    else if (_state == State::Closing && (!_link->GetWaitingSegments() || int32(now - _closeDeadline) >= 0))
        Finish(_closeCode, _closeReason, true);

    return _state != State::Closed;
}

void Connection::Finish(uint16 code, std::string_view reason, bool sendClose)
{
    if (_state == State::Closed)
        return;

    if (sendClose)
        _link->Send(DatagramType::Close, MakeCloseBody(code, reason));

    _state = State::Closed;
    _open = false;
    RT_LOG_DEBUG("network.kcp", "{} conv {} closed{}: {} {}", GetClientInfo(), _conv, sendClose ? "" : " by the client", code, reason);

    try
    {
        OnClose(code, reason);
    }
    catch (std::exception const& e)
    {
        RT_LOG_ERROR("network.kcp", "{} close handler threw: {}", GetClientInfo(), e.what());
    }
}

Server::Server() : _ioContext(1), _socket(_ioContext), _updateTimer(_ioContext)
{
}

Server::~Server()
{
    ASSERT(!_thread, "Stop must be called before the Kcp::Server is destroyed");
}

bool Server::Start(std::string const& bindIp, uint16 port)
{
    ASSERT(!_thread);

    boost::system::error_code error;
    boost::asio::ip::address address = boost::asio::ip::make_address(bindIp, error);
    if (error)
    {
        RT_LOG_ERROR("network.kcp", "Kcp::Server: invalid bind address {}: {}", bindIp, error.message());
        return false;
    }

    IoContextUdpSocket::endpoint_type endpoint(address, port);
    _socket.open(endpoint.protocol(), error);
    if (!error && address.is_v6() && address.is_unspecified())
        _socket.set_option(boost::asio::ip::v6_only(false), error);
    if (!error)
        _socket.bind(endpoint, error);
    if (!error)
        _socket.non_blocking(true, error);
    if (error)
    {
        RT_LOG_ERROR("network.kcp", "Kcp::Server: can't bind {}:{}: {}", bindIp, port, error.message());
        boost::system::error_code closeError;
        _socket.close(closeError);
        return false;
    }

    // Best effort, the system may cap them
    boost::system::error_code ignored;
    _socket.set_option(boost::asio::socket_base::receive_buffer_size(SocketBufferSize), ignored);
    _socket.set_option(boost::asio::socket_base::send_buffer_size(SocketBufferSize), ignored);

    _port = _socket.local_endpoint(ignored).port();

    ReceiveLoop();
    UpdateLoop();
    _thread = std::make_unique<std::thread>([this] { _ioContext.run(); });
    return true;
}

void Server::Stop()
{
    if (!_thread)
        return;

    std::promise<void> done;
    boost::asio::post(_ioContext, [this, &done]
    {
        for (auto& [conv, connection] : _connections)
        {
            connection->Finish(CloseCode::GoingAway, "server stopping", true);
            Remove(*connection);
        }
        _connections.clear();

        boost::system::error_code ignored;
        _socket.close(ignored);
        _updateTimer.cancel();
        done.set_value();
    });
    done.get_future().wait();

    _ioContext.stop();
    _thread->join();
    _thread = nullptr;
}

Rooftree::Detached Server::ReceiveLoop()
{
    for (;;)
    {
        auto [error, size] = co_await Rooftree::Await<boost::system::error_code, std::size_t>([&](auto done)
        {
            _socket.async_receive_from(boost::asio::buffer(_receiveBuffer), _receiveEndpoint, std::move(done));
        });

        if (!_socket.is_open() || error == boost::asio::error::operation_aborted)
            break;

        // An error of one datagram (an ICMP error reported on Windows, a truncated one) isn't the socket's
        if (!error)
            HandleDatagram({ _receiveBuffer.data(), size }, _receiveEndpoint);
    }
}

void Server::HandleDatagram(std::span<uint8 const> datagram, IoContextUdpSocket::endpoint_type const& from)
{
    if (datagram.size() < HeaderSize + TagSize || datagram.size() > MaxDatagramSize || _connectionLimiter.IsBlocked(from.address()))
    {
        ++_dropped;
        return;
    }

    Header header = ReadHeader(datagram);
    if (!header.Conv)
    {
        if (header.Type == DatagramType::Hello)
            HandleHello(datagram, header, from);
        else
            ++_dropped;
        return;
    }

    auto itr = _connections.find(header.Conv);
    if (itr == _connections.end())
    {
        ++_dropped;
        return;
    }

    itr->second->HandleDatagram(datagram, header, from, getMSTime());
}

void Server::HandleHello(std::span<uint8 const> datagram, Header const& header, IoContextUdpSocket::endpoint_type const& from)
{
    std::size_t clearSize = 0;
    std::string_view token = ReadHelloToken(datagram, clearSize);
    if (token.empty())
    {
        ++_dropped;
        return;
    }

    // Repeated: the connection answers it, the token is used up by now
    if (auto itr = _tokens.find(std::string(token)); itr != _tokens.end())
    {
        _connections.at(itr->second)->HandleHello(datagram, header, clearSize, from, getMSTime());
        return;
    }

    if (_maxConnections && _connections.size() >= _maxConnections)
    {
        RT_LOG_DEBUG("network.kcp", "[{}:{}] refused, {} connections open already", from.address(), from.port(), _connections.size());
        ++_dropped;
        return;
    }

    std::shared_ptr<void> ticket;
    if (!_connectionLimiter.TryAcquire(from.address(), ticket))
    {
        RT_LOG_DEBUG("network.kcp", "[{}:{}] refused, the address has {} connections open already", from.address(), from.port(), _connectionLimiter.GetLimit());
        ++_dropped;
        return;
    }

    Optional<Key> verified;
    Optional<Key> otherVersion;
    std::vector<uint8> body;
    auto verify = [&](Key const& key)
    {
        if (!Cipher(key).Open(Direction::ClientToServer, datagram, clearSize, body))
            return false;

        if (body.empty() || body[0] != ProtocolVersion)
        {
            otherVersion = key;
            return false;
        }

        if (body.size() != HelloBodySize)
            return false;

        verified = key;
        return true;
    };

    std::shared_ptr<Connection> connection;
    try
    {
        connection = OnHello(token, from.address(), verify);
    }
    catch (std::exception const& e)
    {
        RT_LOG_ERROR("network.kcp", "[{}:{}] Hello handler threw: {}", from.address(), from.port(), e.what());
    }

    if (otherVersion)
    {
        // It has the key, it may hear why: sealed like a Welcome, the conv still 0
        std::vector<uint8> close;
        Cipher(*otherVersion).Seal(Direction::ServerToClient, { .Conv = 0, .Type = DatagramType::Close, .Counter = RefusalCounter }, {},
            MakeCloseBody(CloseCode::ProtocolError, "unsupported protocol version"), close);
        SendTo(close, from);
        RT_LOG_DEBUG("network.kcp", "[{}:{}] refused, protocol version {} instead of {}", from.address(), from.port(),
            body.empty() ? 0 : body[0], ProtocolVersion);
        ++_dropped;
        return;
    }

    if (!connection || !verified)
    {
        RT_LOG_DEBUG("network.kcp", "[{}:{}] Hello refused", from.address(), from.port());
        ++_dropped;
        return;
    }

    uint32 conv = NewConv();
    connection->Init(*this, conv, *verified, from, std::string(token), body, std::move(ticket), header.Counter);
    _tokens.emplace(token, conv);
    _connections.emplace(conv, connection);
    ++_connectionCount;

    connection->SendWelcome();
    connection->_open = true;
    RT_LOG_DEBUG("network.kcp", "{} conv {} opened", connection->GetClientInfo(), conv);

    try
    {
        connection->OnOpen();
    }
    catch (std::exception const& e)
    {
        RT_LOG_ERROR("network.kcp", "{} open handler threw: {}", connection->GetClientInfo(), e.what());
        connection->Finish(CloseCode::InternalError, "internal error", true);
    }
}

uint32 Server::NewConv() const
{
    uint32 conv;
    do
        conv = urand(1, std::numeric_limits<uint32>::max());
    while (_connections.contains(conv));
    return conv;
}

Rooftree::Detached Server::UpdateLoop()
{
    for (;;)
    {
        _updateTimer.expires_after(1ms);
        auto [error] = co_await Rooftree::Await<boost::system::error_code>([&](auto done) { _updateTimer.async_wait(std::move(done)); });
        if (error)
            break;

        uint32 now = getMSTime();
        std::erase_if(_connections, [this, now](auto& entry)
        {
            if (entry.second->Update(now))
                return false;

            Remove(*entry.second);
            return true;
        });
    }
}

void Server::Remove(Connection& connection)
{
    _tokens.erase(connection._token);
    connection._connectionTicket.reset();
    connection._link.reset();
    --_connectionCount;
}

void Server::SendTo(std::span<uint8 const> datagram, IoContextUdpSocket::endpoint_type const& to)
{
    // Non-blocking: with the socket buffer full it is dropped, like a datagram lost on the way, and
    // KCP sends it again
    boost::system::error_code error;
    _socket.send_to(boost::asio::buffer(datagram.data(), datagram.size()), to, 0, error);
    if (error && error != boost::asio::error::would_block)
        RT_LOG_DEBUG("network.kcp", "send to [{}:{}] failed: {}", to.address(), to.port(), error.message());
}
}
