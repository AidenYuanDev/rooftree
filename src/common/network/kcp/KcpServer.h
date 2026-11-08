/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Game connections over UDP, with KCP for the messages that must arrive (KcpLink.h)
 * and plain datagrams for the ones that may be lost (the newest position replaces the last one
 * anyway). A Server is what SocketMgr and a NetworkThread are for TCP, in one: it owns the port's
 * UDP socket and a thread, and runs every connection on it.
 *
 *   class GameConnection final : public Kcp::Connection
 *   {
 *       void OnMessage(std::string&& message, bool reliable) override { Send(message); }
 *   };
 *
 *   class GameServer final : public Kcp::Server
 *   {
 *       std::shared_ptr<Kcp::Connection> OnHello(std::string_view token, boost::asio::ip::address const&,
 *           std::function<bool(Kcp::Key const&)> const& verify) override
 *       {
 *           // look the token up, take it only if verify(its key)
 *           return std::make_shared<GameConnection>();
 *       }
 *   };
 *
 * UDP has no connections, the Server makes them out of the datagrams (KcpProtocol.h): a Hello with a
 * token and the key it was issued with opens one, the Welcome tells the client its conv, and every
 * datagram after it carries the conv the Server finds the connection by. Nothing that doesn't
 * authenticate is ever answered.
 *
 * A connection follows its client to a new address (a phone switching networks, a NAT that forgot
 * the mapping) when an authenticated datagram newer than any before arrives from there.
 *
 * Threads: the callbacks (OnHello, OnOpen, OnMessage, OnClose) run on the Server's thread; Send,
 * SendUnreliable and Close may be called from any thread (a world thread), they are queued and the
 * thread sends them within a millisecond, like a NetworkThread updates its sockets.
 */

#ifndef ROOFTREE_KCP_SERVER_H
#define ROOFTREE_KCP_SERVER_H

#include "AddressBlocklist.h"
#include "ConnectionLimiter.h"
#include "DeadlineTimer.h"
#include "IoContext.h"
#include "KcpLink.h"
#include "MPSCQueue.h"
#include "Task.h"
#include <boost/asio/ip/udp.hpp>
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

namespace Rooftree::Net::Kcp
{
class Server;

using IoContextUdpSocket = boost::asio::basic_datagram_socket<boost::asio::ip::udp, Asio::IoContextExecutor>;

class Connection : public std::enable_shared_from_this<Connection>
{
public:
    Connection() = default;
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;
    virtual ~Connection();

    // Any thread. A reliable message, false (and nothing sent) if it is larger than MaxMessageSize
    // or the connection is closing
    bool Send(std::string_view message);
    // Any thread. Larger than MaxBodySize goes reliably instead
    void SendUnreliable(std::string_view message);
    // Any thread. The messages sent before go first (up to CloseTimeout), then the Close
    void Close(uint16 code = CloseCode::Normal, std::string_view reason = {});

    // Any thread: from OnOpen until it closed, by either side
    bool IsOpen() const { return _open; }

    uint32 GetConv() const { return _conv; }
    // The address it was opened from
    boost::asio::ip::address const& GetRemoteAddress() const { return _remoteAddress; }
    // [address:port] of the Hello, for logs
    std::string GetClientInfo() const;

protected:
    // Server's thread. The Welcome is sent, messages may be sent from now on
    virtual void OnOpen() { }
    // Server's thread. A message of the client; reliable: sent with Send, in order, else SendUnreliable
    virtual void OnMessage(std::string&& message, bool reliable) = 0;
    // Server's thread, once: the connection is gone, closed by whichever side (code and reason of it)
    virtual void OnClose(uint16 /*code*/, std::string_view /*reason*/) { }

private:
    friend class Server;

    struct Outgoing
    {
        enum Kind : uint8 { Reliable, Unreliable, CloseAfter } Type;
        uint16 Code = 0;
        std::string Data;
    };

    enum class State : uint8
    {
        Open,
        Closing,    // waiting for the reliable data's acknowledgments, then the Close goes
        Closed
    };

    void Enqueue(Outgoing* outgoing);

    // Server's thread from here on
    void Init(Server& server, uint32 conv, Key const& key, IoContextUdpSocket::endpoint_type const& endpoint,
        std::string token, std::span<uint8 const> helloBody, std::shared_ptr<void> connectionTicket, uint64 helloCounter);
    void HandleDatagram(std::span<uint8 const> datagram, Header const& header, IoContextUdpSocket::endpoint_type const& from, uint32 now);
    void HandleHello(std::span<uint8 const> datagram, Header const& header, std::size_t clearSize, IoContextUdpSocket::endpoint_type const& from, uint32 now);
    void Deliver(std::string&& message, bool reliable);
    // The queued sends into KCP, then KCP's timers; false once the connection is to be removed
    bool Update(uint32 now);
    void SendWelcome();
    // Ends it: sends a Close unless the peer closed it, then OnClose
    void Finish(uint16 code, std::string_view reason, bool sendClose);

    Server* _server = nullptr;
    uint32 _conv = 0;
    boost::asio::ip::address _remoteAddress;
    IoContextUdpSocket::endpoint_type _helloEndpoint;
    IoContextUdpSocket::endpoint_type _endpoint;     // where it sends to: the newest datagram's source
    std::string _token;
    std::vector<uint8> _helloBody;      // its nonce tells our client's repeated Hellos from another's
    std::size_t _maxMessageSize = 0;
    std::shared_ptr<void> _connectionTicket;        // ConnectionLimiter's count of the address
    std::unique_ptr<Link> _link;
    std::atomic<bool> _open = false;
    std::atomic<bool> _closeQueued = false;
    State _state = State::Open;
    uint32 _lastReceive = 0;
    uint32 _nextUpdate = 0;
    uint32 _closeDeadline = 0;
    uint16 _closeCode = CloseCode::Normal;
    std::string _closeReason;
    std::atomic<bool> _hasOutgoing = false;
    bool _needsFlush = false;
    MPSCQueue<Outgoing> _outgoing;
    std::vector<uint8> _body;   // scratch for opened datagrams
};

class Server
{
public:
    Server();
    Server(Server const&) = delete;
    Server& operator=(Server const&) = delete;
    virtual ~Server();

    // Before Start
    void SetSettings(Settings const& settings) { _settings = settings; }
    Settings const& GetSettings() const { return _settings; }
    // 0 is no limit
    void SetMaxConnections(uint32 max) { _maxConnections = max; }
    void SetMaxConnectionsPerAddress(uint32 max, std::vector<boost::asio::ip::address> exempt = {}) { _connectionLimiter.SetLimit(max, std::move(exempt)); }
    void SetAddressBlocklist(std::shared_ptr<AddressBlocklist const> blocklist) { _connectionLimiter.SetBlocklist(std::move(blocklist)); }

    // Binds the port (0: any free one, GetPort tells which) and starts the thread
    bool Start(std::string const& bindIp, uint16 port);
    // Drops every connection without telling the clients (close them first) and stops the thread
    void Stop();

    uint16 GetPort() const { return _port; }
    // Any thread
    uint32 GetConnectionCount() const { return _connectionCount; }

    // Any thread: datagrams dropped since the start (too short or large, not authenticating, replayed,
    // unknown conv, refused Hello)
    uint64 GetDroppedDatagrams() const { return _dropped; }

protected:
    // Server's thread: a Hello with this token from address. Find the token's key and call verify with
    // it: true is a Hello sealed with that key. Only then take the token (a login ticket is used up)
    // and answer the connection to open; nullptr refuses the client, it gets no answer
    virtual std::shared_ptr<Connection> OnHello(std::string_view token, boost::asio::ip::address const& address,
        std::function<bool(Key const&)> const& verify) = 0;

private:
    friend class Connection;

    Rooftree::Detached ReceiveLoop();
    void HandleDatagram(std::span<uint8 const> datagram, IoContextUdpSocket::endpoint_type const& from);
    void HandleHello(std::span<uint8 const> datagram, Header const& header, IoContextUdpSocket::endpoint_type const& from);
    Rooftree::Detached UpdateLoop();
    void Remove(Connection& connection);
    void SendTo(std::span<uint8 const> datagram, IoContextUdpSocket::endpoint_type const& to);
    uint32 NewConv() const;

    Settings _settings;
    uint32 _maxConnections = 0;
    ConnectionLimiter _connectionLimiter;

    Asio::IoContext _ioContext;
    IoContextUdpSocket _socket;
    Asio::DeadlineTimer _updateTimer;
    std::unique_ptr<std::thread> _thread;
    uint16 _port = 0;

    std::unordered_map<uint32, std::shared_ptr<Connection>> _connections;   // by conv
    std::unordered_map<std::string, uint32> _tokens;                        // conv a token opened, for repeated Hellos
    std::atomic<uint32> _connectionCount = 0;
    std::atomic<uint64> _dropped = 0;

    IoContextUdpSocket::endpoint_type _receiveEndpoint;
    std::array<uint8, MaxDatagramSize + 1> _receiveBuffer;  // one more to tell a datagram too large
};
}

#endif // ROOFTREE_KCP_SERVER_H
