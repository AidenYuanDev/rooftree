/*
 * Loopback tests for rooftree-network: a SocketMgr based echo server, an HttpService and a WebSocket
 * server, driven by plain blocking boost::asio / Boost.Beast clients on 127.0.0.1.
 */

#include "rooftree_catch2.h"

#include "NetworkTestHelpers.h"

#include "HttpService.h"
#include "HttpSocket.h"
#include "IoContext.h"
#include "Optional.h"
#include "SocketMgr.h"
#include "WebSocketFrame.h"
#include "WebSocketSocket.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/buffers_iterator.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <atomic>
#include <random>
#include <sys/socket.h>
#include <thread>

using namespace std::chrono_literals;
using boost::asio::ip::tcp;

using namespace NetworkTest;

namespace
{
std::atomic<int> EchoClosedConnections;

// Set by a test before it connects, reset by EchoOptionsGuard
struct EchoOptions
{
    Milliseconds Deadline = 0ms;
    Milliseconds IdleTimeout = 0ms;
    std::size_t MaxWriteQueueSize = 0;
} Echo;

struct EchoOptionsGuard
{
    ~EchoOptionsGuard() { Echo = {}; }
};

class EchoSocket final : public Rooftree::Net::Socket<>
{
public:
    using Socket::Socket;

    void Start() override
    {
        SetDeadline(Echo.Deadline);
        SetIdleTimeout(Echo.IdleTimeout);
        SetMaxWriteQueueSize(Echo.MaxWriteQueueSize);
        StartReading();
    }

    Rooftree::Net::SocketReadCallbackResult ReadHandler() override
    {
        MessageBuffer& packet = GetReadBuffer();
        MessageBuffer reply(packet.GetActiveSize());
        reply.Write(packet.GetReadPointer(), packet.GetActiveSize());
        packet.ReadCompleted(packet.GetActiveSize());
        QueuePacket(std::move(reply));
        return Rooftree::Net::SocketReadCallbackResult::KeepReading;
    }

protected:
    void OnClose() override { ++EchoClosedConnections; }
};

class EchoNetworkThread final : public Rooftree::Net::NetworkThread<EchoSocket, EchoNetworkThread> { };

class EchoServer;

struct EchoServerTraits
{
    using Self = EchoServer;
    using SocketType = EchoSocket;
    using ThreadType = EchoNetworkThread;
};

class EchoServer final : public Rooftree::Net::SocketMgr<EchoServerTraits> { };

std::string EchoRoundTrip(tcp::socket& socket, std::string const& message)
{
    boost::asio::write(socket, boost::asio::buffer(message));
    std::string reply(message.size(), '\0');
    boost::asio::read(socket, boost::asio::buffer(reply));
    return reply;
}

// Reads until the server closes the connection (or the 5 s read timeout of Connect() fails it):
// how long that took, or nothing if the server never closed
Optional<std::chrono::milliseconds> WaitForServerClose(tcp::socket& socket)
{
    auto start = std::chrono::steady_clock::now();
    char byte;
    boost::system::error_code error;
    while (!error)
        socket.read_some(boost::asio::buffer(&byte, 1), error);

    if (error != boost::asio::error::eof && error != boost::asio::error::connection_reset)
        return {};
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
}
}

TEST_CASE("SocketMgr echoes data over loopback", "[network]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    EchoServer server;
    REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 2));
    StopNetworkGuard<EchoServer> stopGuard{ context, server };
    EchoClosedConnections = 0;

    boost::asio::io_context clientIo;

    SECTION("several messages on one connection")
    {
        tcp::socket socket = Connect(clientIo, port);
        CHECK(EchoRoundTrip(socket, "hello") == "hello");
        CHECK(EchoRoundTrip(socket, "second message") == "second message");
    }

    SECTION("1 MiB of binary data")
    {
        std::string data(1 << 20, '\0');
        std::mt19937 rng(42);
        for (char& c : data)
            c = char(rng());

        tcp::socket socket = Connect(clientIo, port);
        // Write from another thread: the server echoes while we are still sending, so a single
        // thread doing write-then-read could deadlock on full socket buffers
        std::thread writer([&] { boost::asio::write(socket, boost::asio::buffer(data)); });
        std::string reply(data.size(), '\0');
        boost::system::error_code error;
        boost::asio::read(socket, boost::asio::buffer(reply), error);
        writer.join();
        REQUIRE_FALSE(error);
        CHECK(reply == data);
    }

    SECTION("concurrent clients are spread over the network threads")
    {
        constexpr int ClientCount = 16;
        std::atomic<int> ok = 0;
        std::vector<std::thread> clients;
        for (int i = 0; i < ClientCount; ++i)
        {
            clients.emplace_back([&, i]
            {
                boost::asio::io_context io;
                tcp::socket socket = Connect(io, port);
                std::string message = "client-" + std::to_string(i);
                if (EchoRoundTrip(socket, message) == message)
                    ++ok;
            });
        }
        for (std::thread& client : clients)
            client.join();

        CHECK(ok == ClientCount);
        // Every client closed its socket, the server must notice all of them
        CHECK(WaitFor([] { return EchoClosedConnections == ClientCount; }));
    }
}

TEST_CASE("SocketMgr limits the connections of an address", "[network]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    EchoServer server;
    boost::asio::ip::address loopback = boost::asio::ip::make_address("127.0.0.1");
    boost::asio::io_context clientIo;

    SECTION("over the limit is refused, closing one makes room")
    {
        server.SetMaxConnectionsPerAddress(2);
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 2));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        tcp::socket first = Connect(clientIo, port);
        tcp::socket second = Connect(clientIo, port);
        CHECK(EchoRoundTrip(first, "1") == "1");
        CHECK(EchoRoundTrip(second, "2") == "2");
        CHECK(server.GetConnectionLimiter().GetCount(loopback) == 2);

        // The kernel accepts it, the server closes it right away
        tcp::socket third = Connect(clientIo, port);
        CHECK(WaitForServerClose(third));

        first.close();
        REQUIRE(WaitFor([&] { return server.GetConnectionLimiter().GetCount(loopback) == 1; }));
        tcp::socket fourth = Connect(clientIo, port);
        CHECK(EchoRoundTrip(fourth, "4") == "4");
    }

    SECTION("exempt addresses aren't counted")
    {
        server.SetMaxConnectionsPerAddress(1, { loopback });
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        std::vector<tcp::socket> sockets;
        for (int i = 0; i < 3; ++i)
        {
            sockets.push_back(Connect(clientIo, port));
            CHECK(EchoRoundTrip(sockets.back(), "x") == "x");
        }
        CHECK(server.GetConnectionLimiter().GetCount(loopback) == 0);
    }

    SECTION("a blocked address is refused")
    {
        auto blocklist = std::make_shared<Rooftree::Net::AddressBlocklist>();
        server.SetAddressBlocklist(blocklist);
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        tcp::socket before = Connect(clientIo, port);
        CHECK(EchoRoundTrip(before, "x") == "x");

        blocklist->Block(loopback, TimePoint::clock::now() + 1h);
        tcp::socket blocked = Connect(clientIo, port);
        CHECK(WaitForServerClose(blocked));
    }
}

namespace
{
// A read handler with a bug: the first connection that says "boom"
class ThrowingSocket final : public Rooftree::Net::Socket<>
{
public:
    using Socket::Socket;

    void Start() override { StartReading(); }

    Rooftree::Net::SocketReadCallbackResult ReadHandler() override
    {
        MessageBuffer& packet = GetReadBuffer();
        std::string text(reinterpret_cast<char const*>(packet.GetReadPointer()), packet.GetActiveSize());
        packet.ReadCompleted(packet.GetActiveSize());
        if (text == "boom")
            throw std::runtime_error("a bug in a read handler");

        MessageBuffer reply(text.size());
        reply.Write(text.data(), text.size());
        QueuePacket(std::move(reply));
        return Rooftree::Net::SocketReadCallbackResult::KeepReading;
    }
};

class ThrowingNetworkThread final : public Rooftree::Net::NetworkThread<ThrowingSocket, ThrowingNetworkThread> { };
class ThrowingServer;

struct ThrowingServerTraits
{
    using Self = ThrowingServer;
    using SocketType = ThrowingSocket;
    using ThreadType = ThrowingNetworkThread;
};

class ThrowingServer final : public Rooftree::Net::SocketMgr<ThrowingServerTraits> { };
}

TEST_CASE("What a read handler throws closes its connection, not the server", "[network]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    ThrowingServer server;
    REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<ThrowingServer> stopGuard{ context, server };

    boost::asio::io_context clientIo;
    tcp::socket bystander = Connect(clientIo, port);
    CHECK(EchoRoundTrip(bystander, "hello") == "hello");

    tcp::socket victim = Connect(clientIo, port);
    boost::asio::write(victim, boost::asio::buffer(std::string_view("boom")));
    CHECK(WaitForServerClose(victim));

    CHECK(EchoRoundTrip(bystander, "still here") == "still here");
    tcp::socket next = Connect(clientIo, port);
    CHECK(EchoRoundTrip(next, "new") == "new");
}

TEST_CASE("Socket closes connections that time out or don't read", "[network]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    EchoServer server;
    EchoOptionsGuard optionsGuard;
    EchoClosedConnections = 0;
    boost::asio::io_context clientIo;

    SECTION("idle timeout: reading keeps it open, silence closes it")
    {
        Echo.IdleTimeout = 300ms;
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        tcp::socket socket = Connect(clientIo, port);
        for (int i = 0; i < 5; ++i)
        {
            std::this_thread::sleep_for(100ms);
            CHECK(EchoRoundTrip(socket, "still here") == "still here");
        }

        Optional<std::chrono::milliseconds> closedAfter = WaitForServerClose(socket);
        REQUIRE(closedAfter);
        CHECK(*closedAfter >= 200ms);
        CHECK(*closedAfter < 2s);
    }

    SECTION("a deadline closes it whatever the client sends")
    {
        Echo.Deadline = 300ms;
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        tcp::socket socket = Connect(clientIo, port);
        auto start = std::chrono::steady_clock::now();
        boost::system::error_code error;
        while (!error && std::chrono::steady_clock::now() - start < 3s)
        {
            boost::asio::write(socket, boost::asio::buffer(std::string_view("a")), error);
            char reply;
            if (!error)
                boost::asio::read(socket, boost::asio::buffer(&reply, 1), error);
            std::this_thread::sleep_for(20ms);
        }
        CHECK(error);
        CHECK(std::chrono::steady_clock::now() - start < 2s);
    }

    SECTION("a client that doesn't read is closed once the write queue is full")
    {
        Echo.MaxWriteQueueSize = 256 * 1024;
        REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
        StopNetworkGuard<EchoServer> stopGuard{ context, server };

        tcp::socket socket = Connect(clientIo, port);
        // A write that can't go on fails the test instead of hanging it. No small SO_RCVBUF here: with
        // one, loopback TCP could stall for good (the kernel prunes the client's receive queue, the
        // client never learns the server's window opened again)
        timeval sendTimeout{ .tv_sec = 10, .tv_usec = 0 };
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));

        // Never reads the echo: it piles up in the kernel's buffers first, then in the write queue
        std::string chunk(64 * 1024, 'x');
        boost::system::error_code error;
        for (int i = 0; i < 1024 && !error; ++i)
            boost::asio::write(socket, boost::asio::buffer(chunk), error);
        CHECK(error);
        CHECK(WaitFor([] { return EchoClosedConnections == 1; }));
    }
}

namespace
{
class TestHttpSession;

// What an HTTP socket runs after /upgrade: echoes every message
class EchoConnection final : public Rooftree::Net::WebSocket::Connection
{
protected:
    void OnMessage(Rooftree::Net::WebSocket::Opcode opcode, std::string&& payload) override
    {
        if (opcode == Rooftree::Net::WebSocket::Opcode::Binary)
            SendBinary(payload);
        else
            SendText(payload);
    }
};

// Answers a WebSocket handshake, then runs an EchoConnection
Rooftree::Net::Http::RequestHandlerResult Upgrade(std::shared_ptr<TestHttpSession> session, Rooftree::Net::Http::RequestContext& context);

// Answers from another thread after {ms}, like a handler waiting for the database
Rooftree::Net::Http::RequestHandlerResult RespondLater(std::shared_ptr<TestHttpSession> session, Rooftree::Net::Http::RequestContext& context);

class TestHttpService final : public Rooftree::Net::Http::HttpService<TestHttpSession>
{
public:
    TestHttpService() : HttpService("test")
    {
        RegisterHandler(boost::beast::http::verb::get, "/hello", [](std::shared_ptr<TestHttpSession> /*session*/, Rooftree::Net::Http::RequestContext& context)
        {
            context.response.result(boost::beast::http::status::ok);
            context.response.set(boost::beast::http::field::content_type, "text/plain");
            context.response.body() = "hello";
            return Rooftree::Net::Http::RequestHandlerResult::Handled;
        });

        RegisterHandler(boost::beast::http::verb::post, "/echo", [](std::shared_ptr<TestHttpSession> /*session*/, Rooftree::Net::Http::RequestContext& context)
        {
            context.response.result(boost::beast::http::status::ok);
            context.response.body() = context.request.body();
            return Rooftree::Net::Http::RequestHandlerResult::Handled;
        });

        RegisterHandler(boost::beast::http::verb::put, "/items/{id}", [](std::shared_ptr<TestHttpSession> /*session*/, Rooftree::Net::Http::RequestContext& context)
        {
            context.response.result(boost::beast::http::status::ok);
            context.response.body() = "put " + std::string(context.pathParameters.Get("id"));
            return Rooftree::Net::Http::RequestHandlerResult::Handled;
        });

        // Answers from another thread after a while, like a handler waiting for the database
        RegisterHandler(boost::beast::http::verb::get, "/slow/{ms}", &RespondLater);

        // A WebSocket on the HTTP port
        RegisterHandler(boost::beast::http::verb::get, "/upgrade", &Upgrade);
    }

    // Sessions reach their service through a static, like bnetserver's sLoginService
    static TestHttpService* Current;
    // SetTimeouts of the sessions started from now on
    static Milliseconds RequestTimeout;
    static Milliseconds KeepAliveTimeout;
};

TestHttpService* TestHttpService::Current = nullptr;
Milliseconds TestHttpService::RequestTimeout = 0ms;
Milliseconds TestHttpService::KeepAliveTimeout = 0ms;

class TestHttpSession final : public Rooftree::Net::Http::Socket
{
public:
    using Socket::Socket;

    void Start() override
    {
        SetMaxRequestBodySize(1024);
        SetTimeouts(TestHttpService::RequestTimeout, TestHttpService::KeepAliveTimeout);
        Socket::Start();
    }

    Rooftree::Net::Http::RequestHandlerResult RequestHandler(Rooftree::Net::Http::RequestContext& context) override
    {
        return TestHttpService::Current->HandleRequest(std::static_pointer_cast<TestHttpSession>(shared_from_this()), context);
    }
};

Rooftree::Net::Http::RequestHandlerResult Upgrade(std::shared_ptr<TestHttpSession> session, Rooftree::Net::Http::RequestContext& context)
{
    if (Rooftree::Net::WebSocket::AcceptHandshake(context.request, context.response))
        session->UpgradeToWebSocket(std::make_shared<EchoConnection>(), 0ms, 0);
    return Rooftree::Net::Http::RequestHandlerResult::Handled;
}

Rooftree::Net::Http::RequestHandlerResult RespondLater(std::shared_ptr<TestHttpSession> session, Rooftree::Net::Http::RequestContext& context)
{
    auto asyncContext = std::make_shared<Rooftree::Net::Http::RequestContext>(std::move(context));
    std::thread([session = std::move(session), asyncContext]
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(std::stoi(std::string(asyncContext->pathParameters.Get("ms")))));
        asyncContext->response.result(boost::beast::http::status::ok);
        asyncContext->response.body() = "slow";
        boost::asio::post(session->underlying_stream().get_executor(), [session, asyncContext]
        {
            session->SendResponse(*asyncContext);
        });
    }).detach();
    return Rooftree::Net::Http::RequestHandlerResult::Async;
}

using HttpResponse = boost::beast::http::response<boost::beast::http::string_body>;

HttpResponse SendRequest(tcp::socket& socket, boost::beast::http::verb verb, std::string_view target, std::string body = {})
{
    boost::beast::http::request<boost::beast::http::string_body> request(verb, target, 11);
    request.set(boost::beast::http::field::host, "127.0.0.1");
    request.body() = std::move(body);
    request.prepare_payload();
    boost::beast::http::write(socket, request);

    boost::beast::flat_buffer buffer;
    HttpResponse response;
    boost::beast::http::read(socket, buffer, response);
    return response;
}
}

TEST_CASE("HttpService routes requests over loopback", "[network][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestHttpService service;
    TestHttpService::Current = &service;
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<TestHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    HttpResponse hello = SendRequest(socket, boost::beast::http::verb::get, "/hello");
    CHECK(hello.result() == boost::beast::http::status::ok);
    CHECK(hello.body() == "hello");

    // Same connection (keep-alive) for the following requests
    HttpResponse echo = SendRequest(socket, boost::beast::http::verb::post, "/echo", "ping");
    CHECK(echo.result() == boost::beast::http::status::ok);
    CHECK(echo.body() == "ping");

    CHECK(SendRequest(socket, boost::beast::http::verb::get, "/missing").result() == boost::beast::http::status::not_found);
    // Registered path, wrong method
    HttpResponse wrongMethod = SendRequest(socket, boost::beast::http::verb::get, "/echo");
    CHECK(wrongMethod.result() == boost::beast::http::status::method_not_allowed);
    CHECK(wrongMethod[boost::beast::http::field::allow] == "POST");

    HttpResponse put = SendRequest(socket, boost::beast::http::verb::put, "/items/a%20b");
    CHECK(put.result() == boost::beast::http::status::ok);
    CHECK(put.body() == "put a b");
    CHECK(SendRequest(socket, boost::beast::http::verb::put, "/items/a%2Fb").result() == boost::beast::http::status::bad_request);

    socket.close();

    // The first read of a connection usually completes before its NetworkThread adds it to the sockets
    // it updates, the response closes it right away, and it must still be written
    int answered = 0;
    for (int i = 0; i < 20; ++i)
    {
        tcp::socket fresh = Connect(clientIo, port);
        boost::beast::http::request<boost::beast::http::string_body> request(boost::beast::http::verb::get, "/hello", 11);
        request.set(boost::beast::http::field::host, "127.0.0.1");
        request.keep_alive(false);
        boost::beast::http::write(fresh, request);

        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::system::error_code error;
        boost::beast::http::read(fresh, buffer, response, error);
        if (!error && response.body() == "hello")
            ++answered;
    }
    CHECK(answered == 20);

    SECTION("pipelined requests are answered in order, also after an Async one")
    {
        tcp::socket pipelined = Connect(clientIo, port);
        std::string requests =
            "GET /slow/100 HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /slow/10 HTTP/1.1\r\nHost: x\r\n\r\n"
            "GET /hello HTTP/1.1\r\nHost: x\r\n\r\n";
        boost::asio::write(pipelined, boost::asio::buffer(requests));

        boost::beast::flat_buffer buffer;
        for (std::string_view expected : { "slow", "hello", "slow", "hello" })
        {
            HttpResponse response;
            boost::beast::http::read(pipelined, buffer, response);
            CHECK(response.body() == expected);
        }
    }

    SECTION("a body over the limit is refused with 413 and the connection closed")
    {
        tcp::socket big = Connect(clientIo, port);
        HttpResponse response = SendRequest(big, boost::beast::http::verb::post, "/echo", std::string(2000, 'x'));
        CHECK(response.result() == boost::beast::http::status::payload_too_large);
        CHECK_FALSE(response.keep_alive());
    }

    SECTION("a malformed request gets 400 instead of hanging")
    {
        tcp::socket malformed = Connect(clientIo, port);
        boost::asio::write(malformed, boost::asio::buffer(std::string_view("NOT HTTP AT ALL\r\n\r\n")));
        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::beast::http::read(malformed, buffer, response);
        CHECK(response.result() == boost::beast::http::status::bad_request);
    }
}

namespace
{
namespace WebSocket = Rooftree::Net::WebSocket;

// Echoes every message with its own opcode, closes on "bye" and refuses targets other than /ws
class TestWebSocket final : public WebSocket::BaseSocket<>
{
public:
    explicit TestWebSocket(Rooftree::Net::IoContextTcpSocket&& socket) : BaseSocket(std::move(socket))
    {
        SetTimeouts(HandshakeTimeout, IdleTimeout);
    }

    // SetTimeouts of the connections accepted from now on
    static inline Milliseconds HandshakeTimeout = 0ms;
    static inline Milliseconds IdleTimeout = 0ms;

protected:
    bool OnHandshake(Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response) override
    {
        if (request.target() == "/ws")
            return true;

        response.result(boost::beast::http::status::unauthorized);
        return false;
    }

    void OnMessage(WebSocket::Opcode opcode, std::string&& payload) override
    {
        if (payload == "bye")
            Close(WebSocket::CloseCode::Normal, "see you");
        else if (opcode == WebSocket::Opcode::Binary)
            SendBinary(payload);
        else
            SendText(payload);
    }
};

class TestWebSocketNetworkThread final : public Rooftree::Net::NetworkThread<TestWebSocket, TestWebSocketNetworkThread> { };

class TestWebSocketServer;

struct TestWebSocketServerTraits
{
    using Self = TestWebSocketServer;
    using SocketType = TestWebSocket;
    using ThreadType = TestWebSocketNetworkThread;
};

class TestWebSocketServer final : public Rooftree::Net::SocketMgr<TestWebSocketServerTraits> { };

using WebSocketClient = boost::beast::websocket::stream<tcp::socket>;

std::string ReadMessage(WebSocketClient& client)
{
    boost::beast::flat_buffer buffer;
    client.read(buffer);
    return boost::beast::buffers_to_string(buffer.data());
}
}

TEST_CASE("WebSocket frame codec", "[network][websocket]")
{
    // RFC 6455 section 1.3
    CHECK(WebSocket::ComputeAcceptKey("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");

    auto clientFrame = [](uint8 firstByte, std::string_view payload, bool masked = true)
    {
        std::array<uint8, 4> mask = { 0x37, 0xFA, 0x21, 0x3D };
        MessageBuffer buffer(14 + payload.size());
        std::vector<uint8> bytes;
        bytes.reserve(buffer.GetBufferSize());
        bytes.push_back(firstByte);
        if (payload.size() < 126)
            bytes.push_back(uint8(payload.size()) | (masked ? 0x80 : 0));
        else
        {
            bytes.push_back(126 | (masked ? 0x80 : 0));
            bytes.push_back(uint8(payload.size() >> 8));
            bytes.push_back(uint8(payload.size()));
        }
        if (masked)
            bytes.insert(bytes.end(), mask.begin(), mask.end());
        for (std::size_t i = 0; i < payload.size(); ++i)
            bytes.push_back(uint8(payload[i]) ^ (masked ? mask[i % 4] : 0));
        buffer.Write(bytes.data(), bytes.size());
        return buffer;
    };

    WebSocket::Frame frame;
    WebSocket::CloseCode error = WebSocket::CloseCode::None;

    SECTION("masked text frame")
    {
        MessageBuffer buffer = clientFrame(0x81, "Hello");
        REQUIRE(WebSocket::ParseClientFrame(buffer, frame, 1024, error) == WebSocket::ParseResult::Done);
        CHECK(frame.Fin);
        CHECK(frame.Type == WebSocket::Opcode::Text);
        CHECK(frame.Payload == "Hello");
        CHECK(buffer.GetActiveSize() == 0);
    }

    SECTION("16 bit length, delivered in two parts")
    {
        std::string payload(300, 'x');
        MessageBuffer full = clientFrame(0x82, payload);
        MessageBuffer partial(full.GetActiveSize());
        partial.Write(full.GetReadPointer(), 5);
        CHECK(WebSocket::ParseClientFrame(partial, frame, 1024, error) == WebSocket::ParseResult::NeedMoreData);
        CHECK(partial.GetActiveSize() == 5);
        partial.Write(full.GetReadPointer() + 5, full.GetActiveSize() - 5);
        REQUIRE(WebSocket::ParseClientFrame(partial, frame, 1024, error) == WebSocket::ParseResult::Done);
        CHECK(frame.Payload == payload);
    }

    SECTION("protocol errors")
    {
        MessageBuffer unmasked = clientFrame(0x81, "Hello", false);
        CHECK(WebSocket::ParseClientFrame(unmasked, frame, 1024, error) == WebSocket::ParseResult::Error);
        CHECK(error == WebSocket::CloseCode::ProtocolError);

        MessageBuffer rsv = clientFrame(0xC1, "Hello");
        CHECK(WebSocket::ParseClientFrame(rsv, frame, 1024, error) == WebSocket::ParseResult::Error);

        MessageBuffer fragmentedPing = clientFrame(0x09, "");
        CHECK(WebSocket::ParseClientFrame(fragmentedPing, frame, 1024, error) == WebSocket::ParseResult::Error);

        MessageBuffer tooBig = clientFrame(0x82, std::string(300, 'x'));
        CHECK(WebSocket::ParseClientFrame(tooBig, frame, 100, error) == WebSocket::ParseResult::Error);
        CHECK(error == WebSocket::CloseCode::MessageTooBig);
    }

    SECTION("server frames and close payloads")
    {
        MessageBuffer small = WebSocket::SerializeFrame(WebSocket::Opcode::Text, "Hi");
        REQUIRE(small.GetActiveSize() == 4);
        CHECK(small.GetReadPointer()[0] == 0x81);
        CHECK(small.GetReadPointer()[1] == 2);

        MessageBuffer large = WebSocket::SerializeFrame(WebSocket::Opcode::Binary, std::string(70000, 'x'));
        CHECK(large.GetActiveSize() == 10 + 70000);
        CHECK(large.GetReadPointer()[1] == 127);

        WebSocket::CloseCode code;
        std::string_view reason;
        CHECK(WebSocket::ParseClosePayload(std::string_view("\x03\xE8ok", 4), code, reason));
        CHECK(code == WebSocket::CloseCode::Normal);
        CHECK(reason == "ok");
        CHECK_FALSE(WebSocket::ParseClosePayload(std::string_view("\x03\xED", 2), code, reason)); // 1005 is never sent
        CHECK_FALSE(WebSocket::ParseClosePayload("x", code, reason));
    }
}

// Boost.Beast's websocket client as an independent implementation of the other side
TEST_CASE("WebSocket server talks to a Beast client over loopback", "[network][websocket]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestWebSocketServer server;
    REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 2));
    StopNetworkGuard<TestWebSocketServer> stopGuard{ context, server };

    boost::asio::io_context clientIo;
    WebSocketClient client(Connect(clientIo, port));

    SECTION("text, binary, fragmented and large messages, then a client close")
    {
        client.handshake("127.0.0.1", "/ws");

        client.text(true);
        client.write(boost::asio::buffer(std::string_view("hello")));
        CHECK(ReadMessage(client) == "hello");
        CHECK(client.got_text());

        client.binary(true);
        std::string binary = { '\0', '\x01', '\xFF' };
        client.write(boost::asio::buffer(binary));
        CHECK(ReadMessage(client) == binary);
        CHECK(client.got_binary());

        // Split into frames of 1000 bytes, joined again by the server
        client.text(true);
        client.auto_fragment(true);
        client.write_buffer_bytes(1000);
        std::string large(20000, 'a');
        client.write(boost::asio::buffer(large));
        CHECK(ReadMessage(client) == large);

        client.ping({});
        client.close(boost::beast::websocket::close_code::normal);
    }

    SECTION("server close")
    {
        client.handshake("127.0.0.1", "/ws");
        client.write(boost::asio::buffer(std::string_view("bye")));

        boost::beast::flat_buffer buffer;
        boost::system::error_code error;
        client.read(buffer, error);
        CHECK(error == boost::beast::websocket::error::closed);
        CHECK(client.reason().code == boost::beast::websocket::close_code::normal);
        CHECK(client.reason().reason == "see you");
    }

    SECTION("invalid UTF-8 text closes with 1007")
    {
        client.handshake("127.0.0.1", "/ws");
        // Beast doesn't validate what it sends
        client.write(boost::asio::buffer(std::string_view("\xC3\x28")));

        boost::beast::flat_buffer buffer;
        boost::system::error_code error;
        client.read(buffer, error);
        CHECK(error == boost::beast::websocket::error::closed);
        CHECK(client.reason().code == boost::beast::websocket::close_code::bad_payload);
    }

    SECTION("OnHandshake refuses the connection")
    {
        boost::beast::websocket::response_type response;
        boost::system::error_code error;
        client.handshake(response, "127.0.0.1", "/other", error);
        CHECK(error == boost::beast::websocket::error::upgrade_declined);
        CHECK(response.result() == boost::beast::http::status::unauthorized);
    }
}

TEST_CASE("HttpService closes slow and idle clients", "[network][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestHttpService service;
    TestHttpService::Current = &service;
    TestHttpService::RequestTimeout = 300ms;
    TestHttpService::KeepAliveTimeout = 300ms;
    struct ResetTimeouts
    {
        ~ResetTimeouts() { TestHttpService::RequestTimeout = TestHttpService::KeepAliveTimeout = 0ms; }
    } resetTimeouts;
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<TestHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    SECTION("a connection that sends nothing")
    {
        Optional<std::chrono::milliseconds> closedAfter = WaitForServerClose(socket);
        REQUIRE(closedAfter);
        CHECK(*closedAfter < 2s);
    }

    SECTION("headers a byte at a time: the time for them doesn't restart with every byte")
    {
        std::string request = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Padding: " + std::string(100, 'p') + "\r\n\r\n";
        auto start = std::chrono::steady_clock::now();
        boost::system::error_code error;
        for (char c : request)
        {
            boost::asio::write(socket, boost::asio::buffer(&c, 1), error);
            if (error)
                break;
            std::this_thread::sleep_for(20ms);
        }
        // Either a write failed or the response never comes
        if (!error)
            CHECK(WaitForServerClose(socket));
        CHECK(std::chrono::steady_clock::now() - start < 2s);
    }

    SECTION("an idle keep-alive connection, after its responses")
    {
        CHECK(SendRequest(socket, boost::beast::http::verb::get, "/hello").body() == "hello");
        std::this_thread::sleep_for(150ms);
        CHECK(SendRequest(socket, boost::beast::http::verb::get, "/hello").body() == "hello");

        Optional<std::chrono::milliseconds> closedAfter = WaitForServerClose(socket);
        REQUIRE(closedAfter);
        CHECK(*closedAfter >= 200ms);
    }

    SECTION("a handler slower than the timeouts")
    {
        CHECK(SendRequest(socket, boost::beast::http::verb::get, "/slow/700").body() == "slow");
    }

    SECTION("a body that takes long, but keeps coming")
    {
        std::string body(600, 'b');
        std::string head = "POST /echo HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 600\r\n\r\n";
        boost::asio::write(socket, boost::asio::buffer(head));
        for (int i = 0; i < 6; ++i)
        {
            std::this_thread::sleep_for(150ms);
            boost::asio::write(socket, boost::asio::buffer(body.data() + i * 100, 100));
        }

        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::beast::http::read(socket, buffer, response);
        CHECK(response.body() == body);
    }
}

TEST_CASE("WebSocket server times out handshakes and silent connections", "[network][websocket]")
{
    TestWebSocket::HandshakeTimeout = 300ms;
    TestWebSocket::IdleTimeout = 400ms;
    struct ResetTimeouts
    {
        ~ResetTimeouts() { TestWebSocket::HandshakeTimeout = TestWebSocket::IdleTimeout = 0ms; }
    } resetTimeouts;

    ServerContext context;
    uint16 port = FindFreePort();
    TestWebSocketServer server;
    REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<TestWebSocketServer> stopGuard{ context, server };
    boost::asio::io_context clientIo;

    SECTION("no handshake")
    {
        tcp::socket socket = Connect(clientIo, port);
        Optional<std::chrono::milliseconds> closedAfter = WaitForServerClose(socket);
        REQUIRE(closedAfter);
        CHECK(*closedAfter < 2s);
    }

    SECTION("open: messages keep it open, silence closes it")
    {
        WebSocketClient client(Connect(clientIo, port));
        client.handshake("127.0.0.1", "/ws");
        for (int i = 0; i < 4; ++i)
        {
            std::this_thread::sleep_for(150ms);
            client.write(boost::asio::buffer(std::string_view("ping")));
            CHECK(ReadMessage(client) == "ping");
        }

        boost::beast::flat_buffer buffer;
        boost::system::error_code error;
        client.read(buffer, error);
        CHECK(error);
    }
}

TEST_CASE("HttpService fallback handler and default headers", "[network][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestHttpService service;
    TestHttpService::Current = &service;
    service.SetDefaultHeaders({ { "X-Frame-Options", "DENY" } });
    service.SetFallbackHandler([](std::shared_ptr<Rooftree::Net::Http::AbstractSocket> const&, Rooftree::Net::Http::RequestContext& context)
    {
        context.response.result(boost::beast::http::status::ok);
        context.response.body() = "fallback for " + std::string(context.request.target());
        return Rooftree::Net::Http::RequestHandlerResult::Handled;
    });
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<TestHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    // Routes first, whatever the depth of the path no route has
    HttpResponse hello = SendRequest(socket, boost::beast::http::verb::get, "/hello");
    CHECK(hello.body() == "hello");
    CHECK(hello["X-Frame-Options"] == "DENY");

    HttpResponse deep = SendRequest(socket, boost::beast::http::verb::get, "/a/b/c.html");
    CHECK(deep.body() == "fallback for /a/b/c.html");
    CHECK(deep["X-Frame-Options"] == "DENY");

    // A route's path with another method is still a 405, not the fallback
    CHECK(SendRequest(socket, boost::beast::http::verb::get, "/echo").result() == boost::beast::http::status::method_not_allowed);
}

TEST_CASE("An HTTP socket upgrades to WebSocket", "[network][http][websocket]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestHttpService service;
    TestHttpService::Current = &service;
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<TestHttpService> stopGuard{ context, service };
    boost::asio::io_context clientIo;

    SECTION("HTTP requests first, then the handshake on the same connection")
    {
        tcp::socket socket = Connect(clientIo, port);
        CHECK(SendRequest(socket, boost::beast::http::verb::get, "/hello").body() == "hello");

        boost::beast::websocket::stream<tcp::socket> client(std::move(socket));
        client.handshake("127.0.0.1", "/upgrade");
        client.write(boost::asio::buffer(std::string_view("over the HTTP port")));
        boost::beast::flat_buffer buffer;
        client.read(buffer);
        CHECK(boost::beast::buffers_to_string(buffer.data()) == "over the HTTP port");

        client.close(boost::beast::websocket::close_code::normal);
    }

    SECTION("a frame in the same packet as the handshake")
    {
        tcp::socket socket = Connect(clientIo, port);
        std::string bytes = "GET /upgrade HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n";
        // FIN + text, masked with a zero key: the payload as is
        bytes += std::string("\x81\x82\x00\x00\x00\x00hi", 8);
        boost::asio::write(socket, boost::asio::buffer(bytes));

        boost::asio::streambuf received;
        boost::asio::read_until(socket, received, "\r\n\r\n");
        std::string all(boost::asio::buffers_begin(received.data()), boost::asio::buffers_end(received.data()));
        std::size_t headersEnd = all.find("\r\n\r\n") + 4;
        CHECK(all.starts_with("HTTP/1.1 101"));
        CHECK(all.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") != std::string::npos);

        // The echo: FIN + text, 2 bytes, unmasked
        std::string frame = all.substr(headersEnd);
        while (frame.size() < 4)
        {
            char more[16];
            std::size_t read = socket.read_some(boost::asio::buffer(more));
            frame.append(more, read);
        }
        CHECK(frame.substr(0, 4) == std::string("\x81\x02hi", 4));
    }

    SECTION("a refused upgrade stays HTTP")
    {
        tcp::socket socket = Connect(clientIo, port);
        // No Sec-WebSocket-Key: 400
        boost::beast::http::request<boost::beast::http::string_body> request(boost::beast::http::verb::get, "/upgrade", 11);
        request.set(boost::beast::http::field::host, "127.0.0.1");
        request.set(boost::beast::http::field::upgrade, "websocket");
        request.set(boost::beast::http::field::connection, "Upgrade");
        request.set(boost::beast::http::field::sec_websocket_version, "13");
        boost::beast::http::write(socket, request);
        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::beast::http::read(socket, buffer, response);
        CHECK(response.result() == boost::beast::http::status::bad_request);
        CHECK(SendRequest(socket, boost::beast::http::verb::get, "/hello").body() == "hello");
    }
}
