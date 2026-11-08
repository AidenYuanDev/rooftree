/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The client end of a Kcp::Server connection, for tests and tools, and the reference
 * a game client (Unreal, Unity) implements the protocol after (KcpProtocol.h):
 *
 *   1. log in over HTTPS, get a token and a key
 *   2. Hello with the token, sealed with the key, every HelloInterval until the Welcome comes
 *   3. the Welcome's conv is the connection's: KCP for Send, plain datagrams for SendUnreliable
 *   4. a Ping when nothing was sent for PingInterval, so the server knows the client is still there
 *
 * It runs on an io_context of the caller, which must run it on one thread; the callbacks run there.
 * Send, SendUnreliable and Close may be called from any thread.
 */

#ifndef ROOFTREE_KCP_CLIENT_H
#define ROOFTREE_KCP_CLIENT_H

#include "DeadlineTimer.h"
#include "IoContext.h"
#include "KcpLink.h"
#include "Optional.h"
#include "Task.h"
#include <boost/asio/ip/udp.hpp>
#include <atomic>
#include <memory>

namespace Rooftree::Net::Kcp
{
class Client : public std::enable_shared_from_this<Client>
{
public:
    using Endpoint = boost::asio::ip::udp::endpoint;

    explicit Client(Asio::IoContext& ioContext, Settings const& settings = {});
    Client(Client const&) = delete;
    Client& operator=(Client const&) = delete;
    virtual ~Client();

    // Once. OnOpen once the Welcome arrived, or OnClose (Timeout) if none did in HandshakeTimeout
    void Connect(Endpoint const& server, std::string token, Key const& key);

    // Any thread, once open. False if it isn't or the message is larger than MaxMessageSize
    bool Send(std::string_view message);
    // Any thread, once open. Larger than MaxBodySize goes reliably instead
    void SendUnreliable(std::string_view message);
    // Any thread. The messages sent before go first (up to CloseTimeout), then the Close
    void Close(uint16 code = CloseCode::Normal, std::string_view reason = {});

    // Any thread: from OnOpen until closed
    bool IsOpen() const { return _open; }
    // Any thread: the last Ping's round trip in ms, 0 before the first Pong
    uint32 GetRoundTripTime() const { return _roundTripTime; }
    uint32 GetConv() const { return _conv; }

protected:
    virtual void OnOpen() { }
    virtual void OnMessage(std::string&& message, bool reliable) = 0;
    // Once, however it ended: a Close from the server, Close(), a timeout
    virtual void OnClose(uint16 /*code*/, std::string_view /*reason*/) { }

private:
    enum class State : uint8
    {
        Idle,
        Connecting,     // Hellos until the Welcome
        Open,
        Closing,
        Closed
    };

    // Each holds the client until it is closed
    static Rooftree::Detached ReceiveLoop(std::shared_ptr<Client> self);
    static Rooftree::Detached UpdateLoop(std::shared_ptr<Client> self);
    void HandleDatagram(std::span<uint8 const> datagram);
    void HandleWelcome(std::span<uint8 const> datagram, Header const& header);
    void SendHello();
    void Update();
    void Output(std::span<uint8 const> datagram);
    void Finish(uint16 code, std::string_view reason, bool sendClose);

    Asio::IoContext& _ioContext;
    Settings _settings;
    boost::asio::basic_datagram_socket<boost::asio::ip::udp, Asio::IoContextExecutor> _socket;
    Asio::DeadlineTimer _updateTimer;

    std::string _token;
    Optional<Key> _key;
    State _state = State::Idle;
    std::atomic<bool> _open = false;
    std::atomic<uint32> _conv = 0;
    std::atomic<uint32> _roundTripTime = 0;
    std::unique_ptr<Link> _link;
    uint64 _helloCounter = 0;           // the counters before the Link, Hellos only, from a random start
    std::array<uint8, HelloBodySize> _helloBody;
    std::vector<uint8> _datagram;       // scratch for Hellos
    std::vector<uint8> _body;           // scratch for opened datagrams

    uint32 _connectStart = 0;
    uint32 _lastHello = 0;
    uint32 _lastSend = 0;
    uint32 _lastReceive = 0;
    uint32 _nextUpdate = 0;
    uint32 _closeDeadline = 0;
    uint16 _closeCode = CloseCode::Normal;
    std::string _closeReason;

    std::array<uint8, MaxDatagramSize + 1> _receiveBuffer;
};
}

#endif // ROOFTREE_KCP_CLIENT_H
