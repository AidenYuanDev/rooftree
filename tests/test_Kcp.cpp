/*
 * Tests for Rooftree::Net::Kcp: the datagram format and its crypto, then a Kcp::Server and Kcp::Clients
 * over loopback, directly or through a UDP proxy that loses, reorders, replays datagrams and moves
 * the client to another port.
 */

#include "rooftree_catch2.h"

#include "NetworkTestHelpers.h"

#include "KcpClient.h"
#include "KcpServer.h"
#include <boost/asio/post.hpp>
#include <future>
#include <map>
#include <mutex>
#include <random>

using namespace std::chrono_literals;
using namespace NetworkTest;
namespace Kcp = Rooftree::Net::Kcp;
using boost::asio::ip::udp;

namespace
{
Kcp::Key MakeKey(uint8 seed)
{
    Kcp::Key key;
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = uint8(seed + i * 7);
    return key;
}

std::span<uint8 const> AsBytes(std::string_view text)
{
    return { reinterpret_cast<uint8 const*>(text.data()), text.size() };
}

struct ConnectionLog
{
    std::mutex Lock;
    std::vector<std::pair<std::string, bool>> Messages;
    std::vector<uint16> CloseCodes;
    std::atomic<uint32> Opened = 0;

    std::size_t MessageCount() { std::scoped_lock lock(Lock); return Messages.size(); }
    std::size_t CloseCount() { std::scoped_lock lock(Lock); return CloseCodes.size(); }
    std::vector<uint16> GetCloseCodes() { std::scoped_lock lock(Lock); return CloseCodes; }
};

// Echoes everything the way it came; "close" closes it with 4001, "throw" throws
class EchoConnection final : public Kcp::Connection
{
public:
    explicit EchoConnection(ConnectionLog& log) : _log(log) { }

protected:
    void OnOpen() override { ++_log.Opened; }

    void OnMessage(std::string&& message, bool reliable) override
    {
        {
            std::scoped_lock lock(_log.Lock);
            _log.Messages.emplace_back(message, reliable);
        }

        if (message == "close")
        {
            Send("bye");
            Close(4001, "asked to");
        }
        else if (message == "throw")
            throw std::runtime_error("handler failed");
        else if (message == "order")
        {
            Send("first");
            SendUnreliable("second");
        }
        else if (reliable)
            Send(message);
        else
            SendUnreliable(message);
    }

    void OnClose(uint16 code, std::string_view /*reason*/) override
    {
        std::scoped_lock lock(_log.Lock);
        _log.CloseCodes.push_back(code);
    }

private:
    ConnectionLog& _log;
};

// Tokens are used once, as login tickets are
class EchoServer final : public Kcp::Server
{
public:
    void AddToken(std::string token, Kcp::Key const& key)
    {
        std::scoped_lock lock(_lock);
        _tokens[std::move(token)] = key;
    }

    std::shared_ptr<EchoConnection> LastConnection()
    {
        std::scoped_lock lock(_lock);
        return _last;
    }

    ConnectionLog Log;
    std::atomic<uint32> Hellos = 0;

protected:
    std::shared_ptr<Kcp::Connection> OnHello(std::string_view token, boost::asio::ip::address const& /*address*/,
        std::function<bool(Kcp::Key const&)> const& verify) override
    {
        ++Hellos;
        std::scoped_lock lock(_lock);
        auto itr = _tokens.find(std::string(token));
        if (itr == _tokens.end() || !verify(itr->second))
            return nullptr;

        _tokens.erase(itr);
        _last = std::make_shared<EchoConnection>(Log);
        return _last;
    }

private:
    std::mutex _lock;
    std::unordered_map<std::string, Kcp::Key> _tokens;
    std::shared_ptr<EchoConnection> _last;
};

struct StopServerGuard
{
    ~StopServerGuard() { Server.Stop(); }
    Kcp::Server& Server;
};

class TestClient final : public Kcp::Client
{
public:
    using Client::Client;

    std::vector<std::pair<std::string, bool>> GetMessages()
    {
        std::scoped_lock lock(_lock);
        return _messages;
    }

    std::size_t MessageCount()
    {
        std::scoped_lock lock(_lock);
        return _messages.size();
    }

    Optional<std::pair<uint16, std::string>> GetClose()
    {
        std::scoped_lock lock(_lock);
        return _close;
    }

    std::atomic<bool> Opened = false;

protected:
    void OnOpen() override { Opened = true; }

    void OnMessage(std::string&& message, bool reliable) override
    {
        std::scoped_lock lock(_lock);
        _messages.emplace_back(std::move(message), reliable);
    }

    void OnClose(uint16 code, std::string_view reason) override
    {
        std::scoped_lock lock(_lock);
        _close.emplace(code, std::string(reason));
    }

private:
    std::mutex _lock;
    std::vector<std::pair<std::string, bool>> _messages;
    Optional<std::pair<uint16, std::string>> _close;
};

udp::endpoint Loopback(uint16 port)
{
    return { boost::asio::ip::make_address("127.0.0.1"), port };
}

// Between a client and the server: drops and holds back (reorders) datagrams at random, can repeat
// the last one to the server, and can move to a new upstream port as a NAT that forgot its mapping
class UdpProxy
{
public:
    UdpProxy(uint16 serverPort, double loss, double reorder) : _server(Loopback(serverPort)), _loss(loss), _reorder(reorder),
        _front(_io, Loopback(0)), _upstream(_io)
    {
        OpenUpstream();
        ReceiveFront();
        _thread = std::thread([this] { _io.run(); });
    }

    ~UdpProxy()
    {
        _io.stop();
        _thread.join();
    }

    uint16 GetPort() const { return _front.local_endpoint().port(); }

    void MoveUpstream()
    {
        Run([this]
        {
            _upstream.close();
            OpenUpstream();
        });
    }

    void ReplayLastToServer(int times)
    {
        Run([this, times]
        {
            for (int i = 0; i < times; ++i)
                _upstream.send(boost::asio::buffer(_lastToServer));
        });
    }

    uint16 GetUpstreamPort()
    {
        uint16 port = 0;
        Run([this, &port] { port = _upstream.local_endpoint().port(); });
        return port;
    }

    std::atomic<uint32> Dropped = 0;

private:
    template<typename F>
    void Run(F&& f)
    {
        std::promise<void> done;
        boost::asio::post(_io, [&] { f(); done.set_value(); });
        done.get_future().wait();
    }

    void OpenUpstream()
    {
        _upstream.open(udp::v4());
        _upstream.connect(_server);
        ReceiveUpstream();
    }

    // false: drop it; held: what to send after it, the one held back before
    bool Pass(std::vector<uint8>& datagram, Optional<std::vector<uint8>>& held, std::vector<std::vector<uint8>>& out)
    {
        std::uniform_real_distribution<double> chance(0.0, 1.0);
        if (chance(_random) < _loss)
        {
            ++Dropped;
            return false;
        }

        if (!held && chance(_random) < _reorder)
        {
            held = std::move(datagram);
            return false;
        }

        out.push_back(std::move(datagram));
        if (held)
        {
            out.push_back(std::move(*held));
            held.reset();
        }
        return true;
    }

    void ReceiveFront()
    {
        _front.async_receive_from(boost::asio::buffer(_frontBuffer), _client, [this](boost::system::error_code const& error, std::size_t size)
        {
            if (error)
                return;

            std::vector<uint8> datagram(_frontBuffer.begin(), _frontBuffer.begin() + size);
            _lastToServer = datagram;
            std::vector<std::vector<uint8>> out;
            Pass(datagram, _heldUp, out);
            boost::system::error_code ignored;
            for (std::vector<uint8> const& d : out)
                _upstream.send(boost::asio::buffer(d), 0, ignored);
            ReceiveFront();
        });
    }

    void ReceiveUpstream()
    {
        _upstream.async_receive(boost::asio::buffer(_upstreamBuffer), [this](boost::system::error_code const& error, std::size_t size)
        {
            if (error == boost::asio::error::operation_aborted || !_upstream.is_open())
                return;

            if (!error)
            {
                std::vector<uint8> datagram(_upstreamBuffer.begin(), _upstreamBuffer.begin() + size);
                std::vector<std::vector<uint8>> out;
                Pass(datagram, _heldDown, out);
                boost::system::error_code ignored;
                for (std::vector<uint8> const& d : out)
                    _front.send_to(boost::asio::buffer(d), _client, 0, ignored);
            }
            ReceiveUpstream();
        });
    }

    udp::endpoint _server;
    double _loss;
    double _reorder;
    std::mt19937 _random{ 12345 };
    boost::asio::io_context _io;
    udp::socket _front;
    udp::socket _upstream;
    udp::endpoint _client;
    std::array<uint8, 2048> _frontBuffer;
    std::array<uint8, 2048> _upstreamBuffer;
    std::vector<uint8> _lastToServer;
    Optional<std::vector<uint8>> _heldUp;
    Optional<std::vector<uint8>> _heldDown;
    std::thread _thread;
};

Kcp::Settings FastSettings()
{
    Kcp::Settings settings;
    settings.HandshakeTimeout = 1s;
    settings.HelloInterval = 50ms;
    return settings;
}
}

TEST_CASE("Kcp datagrams", "[network][kcp]")
{
    SECTION("the replay window takes each counter once, out of order within 64")
    {
        Kcp::ReplayWindow window;
        REQUIRE(window.IsNew(0));
        window.Accept(0);
        REQUIRE_FALSE(window.IsNew(0));

        window.Accept(5);
        REQUIRE(window.IsNew(3));
        window.Accept(3);
        REQUIRE_FALSE(window.IsNew(3));
        REQUIRE_FALSE(window.IsNew(5));
        REQUIRE(window.IsNew(4));

        window.Accept(100);
        REQUIRE(window.GetHighest() == 100);
        REQUIRE(window.IsNew(37));
        REQUIRE_FALSE(window.IsNew(36));     // older than the window, whether seen or not
        REQUIRE_FALSE(window.IsNew(5));
        REQUIRE(window.IsNew(101));
    }

    SECTION("sealed datagrams open with the key, the direction and an untouched header")
    {
        Kcp::Cipher cipher(MakeKey(1));
        std::string const message = "hello world";
        std::vector<uint8> datagram;
        cipher.Seal(Kcp::Direction::ClientToServer, { .Conv = 42, .Type = Kcp::DatagramType::Data, .Counter = 7 }, {}, AsBytes(message), datagram);
        REQUIRE(datagram.size() == Kcp::HeaderSize + message.size() + Kcp::TagSize);

        Kcp::Header header = Kcp::ReadHeader(datagram);
        REQUIRE(header.Conv == 42);
        REQUIRE(header.Type == Kcp::DatagramType::Data);
        REQUIRE(header.Counter == 7);

        std::vector<uint8> body;
        REQUIRE(cipher.Open(Kcp::Direction::ClientToServer, datagram, 0, body));
        REQUIRE(std::string(body.begin(), body.end()) == message);

        REQUIRE_FALSE(cipher.Open(Kcp::Direction::ServerToClient, datagram, 0, body));
        REQUIRE_FALSE(Kcp::Cipher(MakeKey(2)).Open(Kcp::Direction::ClientToServer, datagram, 0, body));

        for (std::size_t i : { std::size_t(0), std::size_t(4), std::size_t(8), Kcp::HeaderSize + 2, datagram.size() - 1 })
        {
            std::vector<uint8> tampered = datagram;
            tampered[i] ^= 1;
            REQUIRE_FALSE(cipher.Open(Kcp::Direction::ClientToServer, tampered, 0, body));
        }
    }

    SECTION("a Hello's token is clear but authenticated")
    {
        Kcp::Cipher cipher(MakeKey(3));
        std::string const token = "ticket";
        std::vector<uint8> clear{ uint8(token.size()) };
        clear.insert(clear.end(), token.begin(), token.end());
        uint8 const helloBody[Kcp::HelloBodySize] = { Kcp::ProtocolVersion, 1, 2, 3, 4, 5, 6, 7, 8 };
        std::vector<uint8> datagram;
        cipher.Seal(Kcp::Direction::ClientToServer, { .Conv = 0, .Type = Kcp::DatagramType::Hello, .Counter = 0 }, clear, helloBody, datagram);

        std::size_t clearSize = 0;
        REQUIRE(Kcp::ReadHelloToken(datagram, clearSize) == token);
        REQUIRE(clearSize == clear.size());

        std::vector<uint8> body;
        REQUIRE(cipher.Open(Kcp::Direction::ClientToServer, datagram, clearSize, body));
        REQUIRE(body == std::vector<uint8>(std::begin(helloBody), std::end(helloBody)));

        datagram[Kcp::HeaderSize + 1] = 'T';
        REQUIRE_FALSE(cipher.Open(Kcp::Direction::ClientToServer, datagram, clearSize, body));

        std::vector<uint8> truncated(datagram.begin(), datagram.begin() + Kcp::HeaderSize + 4);
        REQUIRE(Kcp::ReadHelloToken(truncated, clearSize).empty());
    }

    SECTION("close bodies")
    {
        std::vector<uint8> body = Kcp::MakeCloseBody(4001, std::string(300, 'x'));
        uint16 code = 0;
        std::string reason;
        REQUIRE(Kcp::ReadCloseBody(body, code, reason));
        REQUIRE(code == 4001);
        REQUIRE(reason.size() == Kcp::MaxCloseReasonSize);
        REQUIRE_FALSE(Kcp::ReadCloseBody(std::vector<uint8>{ 1 }, code, reason));
    }

    SECTION("the largest message fits KCP's fragments and the receive window")
    {
        Kcp::Settings settings;
        REQUIRE(settings.GetMaxMessageSizeLimit() == 127 * (Kcp::MaxBodySize - 24));
        REQUIRE(settings.MaxMessageSize <= settings.GetMaxMessageSizeLimit());
    }
}

TEST_CASE("Kcp::Server and Kcp::Client over loopback", "[network][kcp]")
{
    ServerContext clientContext;
    EchoServer server;
    REQUIRE(server.Start("127.0.0.1", 0));
    StopServerGuard stopGuard{ server };
    REQUIRE(server.GetPort());

    Kcp::Key key = MakeKey(10);
    server.AddToken("t1", key);

    SECTION("reliable messages arrive in order, unreliable ones as unreliable")
    {
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));
        REQUIRE(server.GetConnectionCount() == 1);
        REQUIRE(client->GetConv() == server.LastConnection()->GetConv());
        REQUIRE(server.LastConnection()->GetRemoteAddress().to_string() == "127.0.0.1");

        for (int i = 0; i < 200; ++i)
            REQUIRE(client->Send("message " + std::to_string(i)));
        client->SendUnreliable("fast");

        // Larger than a datagram: fragments, and an unreliable one goes reliably
        std::string const big(60000, 'b');
        REQUIRE(client->Send(big));
        client->SendUnreliable(std::string(2000, 'u'));

        REQUIRE(WaitFor([&] { return client->MessageCount() == 203; }));
        std::vector<std::pair<std::string, bool>> messages = client->GetMessages();
        std::vector<std::string> reliable;
        for (auto const& [message, isReliable] : messages)
        {
            if (message == "fast")
                REQUIRE_FALSE(isReliable);
            else
            {
                REQUIRE(isReliable);
                reliable.push_back(message);
            }
        }
        REQUIRE(reliable.size() == 202);
        for (int i = 0; i < 200; ++i)
            REQUIRE(reliable[i] == "message " + std::to_string(i));
        REQUIRE(reliable[200] == big);
        REQUIRE(reliable[201] == std::string(2000, 'u'));

        // Too large for any connection: refused at once
        REQUIRE_FALSE(client->Send(std::string(server.GetSettings().MaxMessageSize + 1, 'x')));
    }

    SECTION("without loss what was sent reliably first arrives first")
    {
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        REQUIRE(client->Send("order"));
        REQUIRE(WaitFor([&] { return client->MessageCount() == 2; }));
        std::vector<std::pair<std::string, bool>> messages = client->GetMessages();
        REQUIRE(messages[0] == std::pair<std::string, bool>("first", true));
        REQUIRE(messages[1] == std::pair<std::string, bool>("second", false));
    }

    SECTION("the server closes after what it sent before, the client hears why")
    {
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        REQUIRE(client->Send("close"));
        REQUIRE(WaitFor([&] { return client->GetClose().has_value(); }));
        REQUIRE(client->GetClose()->first == 4001);
        REQUIRE(client->GetClose()->second == "asked to");
        REQUIRE(client->GetMessages().back().first == "bye");
        REQUIRE_FALSE(client->IsOpen());

        REQUIRE(WaitFor([&] { return server.GetConnectionCount() == 0; }));
        REQUIRE(server.Log.GetCloseCodes() == std::vector<uint16>{ 4001 });
        REQUIRE_FALSE(server.LastConnection()->IsOpen());
        REQUIRE_FALSE(server.LastConnection()->Send("too late"));
    }

    SECTION("the client closes, the server hears why")
    {
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));
        REQUIRE(client->Send("last words"));
        client->Close(4100, "leaving");

        REQUIRE(WaitFor([&] { return server.Log.CloseCount() == 1; }));
        REQUIRE(server.Log.GetCloseCodes()[0] == 4100);
        REQUIRE(server.Log.Messages.back().first == "last words");
        REQUIRE(WaitFor([&] { return server.GetConnectionCount() == 0; }));
        REQUIRE(client->GetClose()->first == 4100);
    }

    SECTION("a wrong key, an unknown token or a used one gets no answer")
    {
        auto wrongKey = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        wrongKey->Connect(Loopback(server.GetPort()), "t1", MakeKey(99));
        auto unknown = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        unknown->Connect(Loopback(server.GetPort()), "nope", key);

        REQUIRE(WaitFor([&] { return wrongKey->GetClose() && unknown->GetClose(); }));
        REQUIRE(wrongKey->GetClose()->first == Kcp::CloseCode::Timeout);
        REQUIRE(unknown->GetClose()->first == Kcp::CloseCode::Timeout);
        REQUIRE(server.Log.Opened == 0);
        REQUIRE(server.GetDroppedDatagrams() >= 2);

        // The wrong key didn't use it up
        auto first = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        first->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return first->IsOpen(); }));

        auto second = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        second->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return second->GetClose().has_value(); }));
        REQUIRE(second->GetClose()->first == Kcp::CloseCode::Timeout);
        REQUIRE(server.GetConnectionCount() == 1);
    }

    SECTION("a handler that throws loses its connection only")
    {
        server.AddToken("t2", key);
        auto thrower = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        thrower->Connect(Loopback(server.GetPort()), "t1", key);
        auto other = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        other->Connect(Loopback(server.GetPort()), "t2", key);
        REQUIRE(WaitFor([&] { return thrower->IsOpen() && other->IsOpen(); }));

        REQUIRE(thrower->Send("throw"));
        REQUIRE(WaitFor([&] { return thrower->GetClose().has_value(); }));
        REQUIRE(thrower->GetClose()->first == Kcp::CloseCode::InternalError);

        REQUIRE(other->Send("still here"));
        REQUIRE(WaitFor([&] { return other->MessageCount() == 1; }));
    }

    SECTION("stopping the server closes its clients with 1001")
    {
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "t1", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        server.Stop();
        REQUIRE(WaitFor([&] { return client->GetClose().has_value(); }));
        REQUIRE(client->GetClose()->first == Kcp::CloseCode::GoingAway);
        REQUIRE(server.Log.GetCloseCodes() == std::vector<uint16>{ Kcp::CloseCode::GoingAway });
    }
}

TEST_CASE("Kcp::Server limits", "[network][kcp]")
{
    ServerContext clientContext;
    EchoServer server;
    Kcp::Key key = MakeKey(20);
    for (char const* token : { "a", "b", "c" })
        server.AddToken(token, key);

    SECTION("silence past the idle timeout closes the connection")
    {
        Kcp::Settings settings = FastSettings();
        settings.IdleTimeout = 300ms;
        server.SetSettings(settings);
        REQUIRE(server.Start("127.0.0.1", 0));
        StopServerGuard stopGuard{ server };

        // A client that never pings, and doesn't time out itself
        Kcp::Settings quiet = FastSettings();
        quiet.PingInterval = 60s;
        quiet.IdleTimeout = 60s;
        auto client = std::make_shared<TestClient>(clientContext.Get(), quiet);
        client->Connect(Loopback(server.GetPort()), "a", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        REQUIRE(WaitFor([&] { return client->GetClose().has_value(); }, 3s));
        REQUIRE(client->GetClose()->first == Kcp::CloseCode::Timeout);
        REQUIRE(server.Log.GetCloseCodes() == std::vector<uint16>{ Kcp::CloseCode::Timeout });

        // Pings keep one open
        Kcp::Settings pinging = FastSettings();
        pinging.PingInterval = 50ms;
        auto alive = std::make_shared<TestClient>(clientContext.Get(), pinging);
        alive->Connect(Loopback(server.GetPort()), "b", key);
        REQUIRE(WaitFor([&] { return alive->IsOpen(); }));
        std::this_thread::sleep_for(800ms);
        REQUIRE(alive->IsOpen());
        REQUIRE(alive->GetRoundTripTime() < 100);
    }

    SECTION("a message over the server's limit closes with 1009")
    {
        Kcp::Settings settings = FastSettings();
        settings.MaxMessageSize = 1000;
        server.SetSettings(settings);
        REQUIRE(server.Start("127.0.0.1", 0));
        StopServerGuard stopGuard{ server };

        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "a", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));
        REQUIRE(client->Send(std::string(1000, 'x')));
        REQUIRE(WaitFor([&] { return client->MessageCount() == 1; }));
        REQUIRE(client->Send(std::string(3000, 'x')));
        REQUIRE(WaitFor([&] { return client->GetClose().has_value(); }));
        REQUIRE(client->GetClose()->first == Kcp::CloseCode::MessageTooBig);
    }

    SECTION("connections per address and in all")
    {
        server.SetMaxConnectionsPerAddress(1);
        REQUIRE(server.Start("127.0.0.1", 0));
        StopServerGuard stopGuard{ server };

        auto first = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        first->Connect(Loopback(server.GetPort()), "a", key);
        REQUIRE(WaitFor([&] { return first->IsOpen(); }));

        auto second = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        second->Connect(Loopback(server.GetPort()), "b", key);
        REQUIRE(WaitFor([&] { return second->GetClose().has_value(); }));
        REQUIRE(second->GetClose()->first == Kcp::CloseCode::Timeout);
        REQUIRE(server.Hellos == 1);    // refused before its token was looked at

        // Closing the first makes room
        first->Close();
        REQUIRE(WaitFor([&] { return server.GetConnectionCount() == 0; }));
        auto third = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        third->Connect(Loopback(server.GetPort()), "b", key);
        REQUIRE(WaitFor([&] { return third->IsOpen(); }));
    }

    SECTION("a blocked address is ignored")
    {
        auto blocklist = std::make_shared<Rooftree::Net::AddressBlocklist>();
        blocklist->Block(boost::asio::ip::make_address("127.0.0.1"), TimePoint::clock::now() + 1h);
        server.SetAddressBlocklist(blocklist);
        REQUIRE(server.Start("127.0.0.1", 0));
        StopServerGuard stopGuard{ server };

        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(server.GetPort()), "a", key);
        REQUIRE(WaitFor([&] { return client->GetClose().has_value(); }));
        REQUIRE(server.Hellos == 0);
    }

    SECTION("a client of another protocol version is told so")
    {
        REQUIRE(server.Start("127.0.0.1", 0));
        StopServerGuard stopGuard{ server };

        std::vector<uint8> clear{ 1, 'a' };
        uint8 const helloBody[Kcp::HelloBodySize] = { Kcp::ProtocolVersion + 1 };
        std::vector<uint8> hello;
        Kcp::Cipher(key).Seal(Kcp::Direction::ClientToServer, { .Conv = 0, .Type = Kcp::DatagramType::Hello, .Counter = 0 }, clear, helloBody, hello);

        boost::asio::io_context io;
        udp::socket socket(io, Loopback(0));
        timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        socket.send_to(boost::asio::buffer(hello), Loopback(server.GetPort()));

        std::array<uint8, 2048> buffer;
        std::size_t size = socket.receive(boost::asio::buffer(buffer));
        std::span<uint8 const> answer(buffer.data(), size);
        REQUIRE(Kcp::ReadHeader(answer).Type == Kcp::DatagramType::Close);
        std::vector<uint8> body;
        REQUIRE(Kcp::Cipher(key).Open(Kcp::Direction::ServerToClient, answer, 0, body));
        uint16 code = 0;
        std::string reason;
        REQUIRE(Kcp::ReadCloseBody(body, code, reason));
        REQUIRE(code == Kcp::CloseCode::ProtocolError);
        REQUIRE(server.GetConnectionCount() == 0);

        // Its token isn't used up: the connection that follows under this key doesn't reuse the Close's nonce
        uint64 const closeCounter = Kcp::ReadHeader(answer).Counter;
        uint8 const rightBody[Kcp::HelloBodySize] = { Kcp::ProtocolVersion };
        Kcp::Cipher(key).Seal(Kcp::Direction::ClientToServer, { .Conv = 0, .Type = Kcp::DatagramType::Hello, .Counter = 1 }, clear, rightBody, hello);
        socket.send_to(boost::asio::buffer(hello), Loopback(server.GetPort()));

        size = socket.receive(boost::asio::buffer(buffer));
        answer = { buffer.data(), size };
        REQUIRE(Kcp::ReadHeader(answer).Type == Kcp::DatagramType::Welcome);
        REQUIRE(Kcp::ReadHeader(answer).Counter != closeCounter);
    }

    SECTION("clients given the same key don't use the same counters")
    {
        boost::asio::io_context io;
        udp::socket socket(io, Loopback(0));
        timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        auto first = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        auto second = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        first->Connect(Loopback(socket.local_endpoint().port()), "a", key);
        second->Connect(Loopback(socket.local_endpoint().port()), "a", key);

        // The first Hello of each, by the port it comes from
        std::map<uint16, uint64> counters;
        std::array<uint8, 2048> buffer;
        while (counters.size() < 2)
        {
            udp::endpoint from;
            std::size_t size = socket.receive_from(boost::asio::buffer(buffer), from);
            counters.emplace(from.port(), Kcp::ReadHeader({ buffer.data(), size }).Counter);
        }

        uint64 const a = counters.begin()->second;
        uint64 const b = counters.rbegin()->second;
        REQUIRE((a > b ? a - b : b - a) > 1000);
    }
}

TEST_CASE("Kcp over a bad network", "[network][kcp]")
{
    ServerContext clientContext;
    EchoServer server;
    REQUIRE(server.Start("127.0.0.1", 0));
    StopServerGuard stopGuard{ server };
    Kcp::Key key = MakeKey(30);
    server.AddToken("t", key);

    SECTION("20% loss and reordering both ways: every reliable message, in order")
    {
        UdpProxy proxy(server.GetPort(), 0.2, 0.1);
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(proxy.GetPort()), "t", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        for (int i = 0; i < 300; ++i)
            REQUIRE(client->Send("m" + std::to_string(i)));
        REQUIRE(client->Send(std::string(20000, 'z')));

        // Well under a second. A stall shows as 7 s or more: a receive window of 0 advertised and KCP's
        // window probe waited for (the client acknowledged before it took the messages)
        REQUIRE(WaitFor([&] { return client->MessageCount() == 301; }, 5s));
        std::vector<std::pair<std::string, bool>> messages = client->GetMessages();
        for (int i = 0; i < 300; ++i)
            REQUIRE(messages[i].first == "m" + std::to_string(i));
        REQUIRE(messages[300].first == std::string(20000, 'z'));
        REQUIRE(proxy.Dropped > 0);
    }

    SECTION("replayed datagrams are dropped")
    {
        UdpProxy proxy(server.GetPort(), 0.0, 0.0);
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(proxy.GetPort()), "t", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));

        client->SendUnreliable("once");
        REQUIRE(WaitFor([&] { return server.Log.MessageCount() == 1; }));
        uint64 dropped = server.GetDroppedDatagrams();
        proxy.ReplayLastToServer(5);
        REQUIRE(WaitFor([&] { return server.GetDroppedDatagrams() == dropped + 5; }));
        REQUIRE(server.Log.MessageCount() == 1);
    }

    SECTION("the connection follows the client to a new address")
    {
        UdpProxy proxy(server.GetPort(), 0.0, 0.0);
        auto client = std::make_shared<TestClient>(clientContext.Get(), FastSettings());
        client->Connect(Loopback(proxy.GetPort()), "t", key);
        REQUIRE(WaitFor([&] { return client->IsOpen(); }));
        REQUIRE(client->Send("before"));
        REQUIRE(WaitFor([&] { return client->MessageCount() == 1; }));

        uint16 oldPort = proxy.GetUpstreamPort();
        proxy.MoveUpstream();
        REQUIRE(proxy.GetUpstreamPort() != oldPort);

        REQUIRE(client->Send("after"));
        REQUIRE(WaitFor([&] { return client->MessageCount() == 2; }));
        REQUIRE(client->GetMessages()[1].first == "after");
        REQUIRE(server.GetConnectionCount() == 1);
        // Logged as from where it opened
        REQUIRE(server.LastConnection()->GetClientInfo() == "[127.0.0.1:" + std::to_string(oldPort) + "]");
    }
}
