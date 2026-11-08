/*
 * rooftree-server's front ends over loopback: WorldHttpService and WorldWebSocket feeding a SessionMgr
 * that a test thread updates like a server's main loop, and Mailer talking to a minimal SMTP server.
 */

#include "rooftree_catch2.h"

#include "HttpRequestUtils.h"
#include "Mailer.h"
#include "NetworkTestHelpers.h"
#include "SessionMgr.h"
#include "Packets_generated.h"
#include "TestPackets_generated.h"
#include "WorldHttpService.h"
#include "WorldWebSocket.h"
#include <boost/asio/read_until.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/beast/websocket/stream.hpp>
#include <boost/json/serialize.hpp>
#include <boost/asio/ssl/stream.hpp>
#include <boost/beast/ssl.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <boost/beast/core/ostream.hpp>
#include <filesystem>
#include <random>
#include <sstream>

using namespace NetworkTest;
using namespace Rooftree::Server;
namespace http = boost::beast::http;

namespace
{
std::atomic<std::thread::id> MainThreadId;
std::atomic<int> HandlersOffMainThread = 0;

void CheckMainThread()
{
    if (std::this_thread::get_id() != MainThreadId.load())
        ++HandlersOffMainThread;
}

std::string StreamPattern(std::size_t size)
{
    std::string data(size, '\0');
    for (std::size_t i = 0; i < size; ++i)
        data[i] = char('a' + i % 26);
    return data;
}

class TestServerSession final : public Session
{
public:
    using Session::Session;

    void HandleHello(WorldHttpRequestPtr const& request)
    {
        CheckMainThread();
        request->RespondJson(http::status::ok, {
            { "hello", request->GetPathParameter("name") },
            { "session", GetKey() },
            { "client", request->GetClientAddress().to_string() },
        });
    }

    void HandleEcho(WorldHttpRequestPtr const& request)
    {
        Optional<boost::json::object> body = ParseJsonObject(request->GetBody());
        if (!body)
            return request->RespondError(http::status::bad_request, "invalid_json", {}, { { "body", "not_an_object" } });

        request->RespondJson(http::status::ok, *body);
    }

    void HandleForget(WorldHttpRequestPtr const& /*request*/) { }

    void HandleThrow(WorldHttpRequestPtr const& /*request*/) { throw std::runtime_error("a bug"); }

    Rooftree::Task<void> HandleThrowOnWorker(WorldHttpRequestPtr request)
    {
        co_await OnWorker([]() -> int { throw std::runtime_error("a bug on a worker"); });
        request->RespondJson(http::status::ok, {});
    }

    Rooftree::Task<void> HandleWork(WorldHttpRequestPtr request)
    {
        int result = co_await OnWorker([] { return 6 * 7; });
        CheckMainThread();
        request->RespondJson(http::status::ok, { { "result", result } });
    }

    // ?size= bytes of a pattern, streamed
    void HandleStream(WorldHttpRequestPtr const& request)
    {
        std::size_t size = std::stoul(request->GetQueryParameter("size").value_or("0"));
        request->GetResponse().set(http::field::content_type, "application/octet-stream");
        request->RespondStream(http::status::ok, std::make_shared<std::istringstream>(StreamPattern(size)), size);
    }

    // Says it has more than the stream holds
    void HandleShortStream(WorldHttpRequestPtr const& request)
    {
        request->RespondStream(http::status::ok, std::make_shared<std::istringstream>(StreamPattern(1000)), 2000);
    }

    void HandleWsEcho(WebSocketJsonMessage& message)
    {
        CheckMainThread();
        message.Reply("echo", message.GetBody());
    }

    void HandleWsWhoAmI(WebSocketJsonMessage& message)
    {
        message.Reply("whoami", { { "session", GetKey() } });
    }

    // The text back, with the session's key after it
    void HandlePing(WebSocketPacketMessage& message, Test::Ping const& ping)
    {
        CheckMainThread();
        flatbuffers::FlatBufferBuilder builder;
        builder.Finish(Test::CreatePing(builder, builder.CreateString(ping.text()->str() + " " + GetKey())));
        message.Reply(builder);
    }
};

class TestServerSessionMgr final : public SessionMgr
{
protected:
    std::unique_ptr<Session> CreateSession(Message const& firstMessage) override
    {
        return std::make_unique<TestServerSession>(*this, firstMessage.GetSessionKey(), firstMessage.GetRemoteAddress());
    }
};

// The server's main thread: updates the sessions until stopped
class MainLoop
{
public:
    explicit MainLoop(SessionMgr& mgr) : _mgr(mgr), _thread([this]
    {
        MainThreadId = std::this_thread::get_id();
        while (!_stop)
        {
            _mgr.Update(1);
            std::this_thread::sleep_for(1ms);
        }
    }) { }

    ~MainLoop() { Stop(); }

    void Stop()
    {
        _stop = true;
        if (_thread.joinable())
            _thread.join();
    }

private:
    SessionMgr& _mgr;
    std::atomic<bool> _stop = false;
    std::thread _thread;
};

using HttpResponse = http::response<http::string_body>;

HttpResponse Send(tcp::socket& socket, http::verb verb, std::string_view target, std::string body = {}, std::string_view forwardedFor = {})
{
    http::request<http::string_body> request(verb, target, 11);
    request.set(http::field::host, "127.0.0.1");
    if (!forwardedFor.empty())
        request.set(http::field::x_forwarded_for, forwardedFor);
    request.body() = std::move(body);
    request.prepare_payload();
    http::write(socket, request);

    boost::beast::flat_buffer buffer;
    HttpResponse response;
    http::read(socket, buffer, response);
    return response;
}

boost::json::object JsonOf(HttpResponse const& response)
{
    boost::json::value value = boost::json::parse(response.body());
    REQUIRE(value.is_object());
    return value.as_object();
}
}

TEST_CASE("Session keys of client addresses", "[server][http]")
{
    auto key = [](char const* address) { return WorldHttpService::AddressSessionKey(boost::asio::ip::make_address(address)); };
    CHECK(key("203.0.113.7") == "ip:203.0.113.7");
    CHECK(key("::ffff:203.0.113.7") == "ip:203.0.113.7");
    // Every address of a /64 is one client
    CHECK(key("2001:db8:1:2:aaaa:bbbb:cccc:dddd") == "ip:2001:db8:1:2::/64");
    CHECK(key("2001:db8:1:2::1") == key("2001:db8:1:2:ffff::"));
    CHECK(key("2001:db8:1:3::1") != key("2001:db8:1:2::1"));
    CHECK(key("::1") == "ip:::/64");
}

TEST_CASE("WorldHttpService runs requests in sessions on the main thread", "[server][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WorldHttpService service("test", sessions);
    service.SetSettings({ .TrustedProxies = { boost::asio::ip::make_address("127.0.0.1") } });
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);
    service.Route(http::verb::post, "/echo", "test.echo", &TestServerSession::HandleEcho);
    service.Route(http::verb::get, "/forget", "test.forget", &TestServerSession::HandleForget);
    service.Route(http::verb::get, "/work", "test.work", &TestServerSession::HandleWork);
    service.Route(http::verb::get, "/limited", "test.limited", &TestServerSession::HandleHello);
    service.Route(http::verb::get, "/throw", "test.throw", &TestServerSession::HandleThrow);
    service.Route(http::verb::get, "/throw-on-worker", "test.throw", &TestServerSession::HandleThrowOnWorker);

    AntiDosSettings antiDos;
    antiDos.Limits["test.limited"] = { .Window = 1h, .MaxCount = 2 };
    sessions.LoadSettings({}, antiDos);

    HandlersOffMainThread = 0;
    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 2));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    HttpResponse hello = Send(socket, http::verb::get, "/hello/%E4%B8%96%E7%95%8C");
    CHECK(hello.result() == http::status::ok);
    CHECK(hello[http::field::content_type] == "application/json; charset=utf-8");
    boost::json::object body = JsonOf(hello);
    CHECK(body["hello"] == "\xE4\xB8\x96\xE7\x95\x8C");
    CHECK(body["session"] == "ip:127.0.0.1");

    // Behind the trusted proxy the client is the entry before it
    boost::json::object proxied = JsonOf(Send(socket, http::verb::get, "/hello/x", {}, "198.51.100.7, 203.0.113.5, 127.0.0.1"));
    CHECK(proxied["client"] == "203.0.113.5");
    CHECK(proxied["session"] == "ip:203.0.113.5");

    HttpResponse echo = Send(socket, http::verb::post, "/echo", R"({"a":1})");
    CHECK(echo.result() == http::status::ok);
    CHECK(echo.body() == R"({"a":1})");

    HttpResponse invalid = Send(socket, http::verb::post, "/echo", "nope");
    CHECK(invalid.result() == http::status::bad_request);
    CHECK(invalid.body() == R"({"error":{"code":"invalid_json","fields":{"body":"not_an_object"}}})");

    HttpResponse forgotten = Send(socket, http::verb::get, "/forget");
    CHECK(forgotten.result() == http::status::internal_server_error);
    CHECK(JsonOf(forgotten)["error"].as_object()["code"] == "internal_error");

    CHECK(JsonOf(Send(socket, http::verb::get, "/work"))["result"] == 42);

    // A handler that throws, on the main thread or on a worker: that request gets 500, the server goes on
    CHECK(Send(socket, http::verb::get, "/throw").result() == http::status::internal_server_error);
    CHECK(Send(socket, http::verb::get, "/throw-on-worker").result() == http::status::internal_server_error);
    CHECK(Send(socket, http::verb::get, "/hello/after").result() == http::status::ok);

    CHECK(Send(socket, http::verb::get, "/limited").result() == http::status::ok);
    CHECK(Send(socket, http::verb::get, "/limited").result() == http::status::ok);
    HttpResponse limited = Send(socket, http::verb::get, "/limited");
    CHECK(limited.result() == http::status::too_many_requests);
    CHECK(limited[http::field::retry_after] == "60");
    CHECK(JsonOf(limited)["error"].as_object()["code"] == "rate_limited");

    // Routing and 404s still happen on the network thread
    CHECK(Send(socket, http::verb::get, "/missing").result() == http::status::not_found);
    CHECK(Send(socket, http::verb::delete_, "/echo").result() == http::status::method_not_allowed);

    // Many clients at once all get answered. No Catch2 assertions on these threads (JsonOf's REQUIRE):
    // before 3.9 they aren't thread safe, two at once abort the test
    std::atomic<int> answered = 0;
    std::vector<std::thread> clients;
    for (int i = 0; i < 8; ++i)
    {
        clients.emplace_back([&, i]
        {
            boost::asio::io_context io;
            tcp::socket client = Connect(io, port);
            for (int j = 0; j < 10; ++j)
            {
                std::string name = std::to_string(i) + "-" + std::to_string(j);
                boost::json::value body = boost::json::parse(Send(client, http::verb::get, "/hello/" + name).body());
                if (body.is_object() && body.as_object()["hello"] == boost::json::value(name))
                    ++answered;
            }
        });
    }
    for (std::thread& client : clients)
        client.join();
    CHECK(answered == 80);

    CHECK(HandlersOffMainThread == 0);

    socket.close();
}

TEST_CASE("WorldHttpService drops a blocked client, answers 429 for one behind a proxy", "[server][http][antidos]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WorldHttpService service("test", sessions);
    service.SetSettings({ .TrustedProxies = { boost::asio::ip::make_address("127.0.0.1") } });
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);
    sessions.LoadSettings({}, {});

    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);
    CHECK(Send(socket, http::verb::get, "/hello/x").result() == http::status::ok);

    // Through the proxy: only the client is blocked, the connection stays
    sessions.GetAddressBlocklist()->Block(boost::asio::ip::make_address("203.0.113.5"), TimePoint::clock::now() + 1h);
    HttpResponse proxied = Send(socket, http::verb::get, "/hello/x", {}, "203.0.113.5");
    CHECK(proxied.result() == http::status::too_many_requests);
    CHECK(JsonOf(proxied)["error"].as_object()["code"] == "rate_limited");
    CHECK(Send(socket, http::verb::get, "/hello/x", {}, "203.0.113.6").result() == http::status::ok);

    // Directly: its open connection is closed at its next request, without an answer
    sessions.GetAddressBlocklist()->Block(boost::asio::ip::make_address("127.0.0.1"), TimePoint::clock::now() + 1h);
    CHECK_THROWS(Send(socket, http::verb::get, "/hello/x"));
}

TEST_CASE("WorldHttpService streams a response body", "[server][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WorldHttpService service("test", sessions);
    service.Route(http::verb::get, "/stream", "test.stream", &TestServerSession::HandleStream);
    service.Route(http::verb::get, "/short", "test.stream", &TestServerSession::HandleShortStream);
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);

    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    // Many pieces, more than is ever queued at once; the connection takes the next request after it
    std::size_t size = 5 * 1024 * 1024 + 123;
    HttpResponse big = Send(socket, http::verb::get, "/stream?size=" + std::to_string(size));
    CHECK(big.result() == http::status::ok);
    CHECK(big[http::field::content_length] == std::to_string(size));
    CHECK(big.body() == StreamPattern(size));
    CHECK(Send(socket, http::verb::get, "/hello/again").result() == http::status::ok);

    HttpResponse empty = Send(socket, http::verb::get, "/stream?size=0");
    CHECK(empty.result() == http::status::ok);
    CHECK(empty.body().empty());

    // Two requests at once: the second is answered after the whole body of the first
    {
        http::request<http::string_body> first(http::verb::get, "/stream?size=600000", 11);
        first.set(http::field::host, "127.0.0.1");
        http::request<http::string_body> second(http::verb::get, "/hello/second", 11);
        second.set(http::field::host, "127.0.0.1");
        boost::beast::flat_buffer out;
        boost::beast::ostream(out) << first << second;
        boost::asio::write(socket, out.data());
        boost::beast::flat_buffer buffer;
        HttpResponse one;
        HttpResponse two;
        http::read(socket, buffer, one);
        http::read(socket, buffer, two);
        CHECK(one.body() == StreamPattern(600000));
        CHECK(JsonOf(two)["hello"] == "second");
    }

    // A stream that ends early can't change the status any more: the connection is closed
    {
        tcp::socket other = Connect(clientIo, port);
        http::request<http::string_body> request(http::verb::get, "/short", 11);
        request.set(http::field::host, "127.0.0.1");
        http::write(other, request);
        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::system::error_code error;
        http::read(other, buffer, response, error);
        CHECK(error);
    }

    socket.close();
}

namespace
{
// /ws?user=<name> connects to the session "user:<name>", no user is refused
std::shared_ptr<Rooftree::Net::WebSocket::Connection> MakeUserWebSocket(WebSocketRouter const& router, Rooftree::Net::Http::Request const& request,
    Rooftree::Net::Http::Response& response, boost::asio::ip::address const& client)
{
    Optional<std::string> user = Rooftree::Net::Http::GetQueryParameter(Rooftree::Net::Http::ToStdStringView(request.target()), "user");
    if (!user)
    {
        response.result(http::status::unauthorized);
        return nullptr;
    }

    return std::make_shared<WorldWebSocket>(router, client, "user:" + *user);
}

using WebSocketClient = boost::beast::websocket::stream<tcp::socket>;

boost::json::object Exchange(WebSocketClient& client, std::string_view message)
{
    client.write(boost::asio::buffer(message));
    boost::beast::flat_buffer buffer;
    client.read(buffer);
    boost::json::value value = boost::json::parse(boost::beast::buffers_to_string(buffer.data()));
    REQUIRE(value.is_object());
    return value.as_object();
}
}

TEST_CASE("WorldWebSocket routes JSON messages to the connection's session", "[server][websocket]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WebSocketRouter router(sessions);
    router.Add("echo", &TestServerSession::HandleWsEcho);
    router.Add("whoami", &TestServerSession::HandleWsWhoAmI);
    router.Add("spam", &TestServerSession::HandleWsEcho);

    AntiDosSettings antiDos;
    antiDos.Limits["ws.spam"] = { .Window = 1h, .MaxCount = 1 };
    sessions.LoadSettings({}, antiDos);

    // On the HTTP service's own port, next to its routes
    WorldHttpService service("test", sessions);
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);
    service.WebSocketRoute("/ws", [&router](Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response,
        boost::asio::ip::address const& client)
    {
        return MakeUserWebSocket(router, request, response, client);
    });

    HandlersOffMainThread = 0;
    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    WebSocketClient client(Connect(clientIo, port));
    client.handshake("127.0.0.1", "/ws?user=alice");

    // The port still answers HTTP
    tcp::socket plain = Connect(clientIo, port);
    CHECK(JsonOf(Send(plain, http::verb::get, "/hello/x"))["hello"] == "x");
    // A GET of the WebSocket path without a handshake
    CHECK(Send(plain, http::verb::get, "/ws").result() == http::status::bad_request);

    boost::json::object echo = Exchange(client, R"({"type":"echo","text":"hi"})");
    CHECK(echo["type"] == "echo");
    CHECK(echo["text"] == "hi");

    CHECK(Exchange(client, R"({"type":"whoami"})")["session"] == "user:alice");

    boost::json::object unknown = Exchange(client, R"({"type":"nope"})");
    CHECK(unknown["type"] == "error");
    CHECK(unknown["code"] == "unknown_type");

    CHECK(Exchange(client, R"({"type":"spam"})")["type"] == "echo");
    boost::json::object limited = Exchange(client, R"({"type":"spam"})");
    CHECK(limited["code"] == "rate_limited");
    CHECK(limited["for"] == "spam");

    // Not a JSON object with a type: closed with 1007
    client.write(boost::asio::buffer(std::string_view("[1,2]")));
    boost::beast::flat_buffer buffer;
    boost::system::error_code error;
    client.read(buffer, error);
    CHECK(error == boost::beast::websocket::error::closed);
    CHECK(client.reason().code == 1007);

    // No user, no connection
    WebSocketClient refused(Connect(clientIo, port));
    boost::beast::websocket::response_type response;
    refused.handshake(response, "127.0.0.1", "/ws", error);
    CHECK(error);
    CHECK(response.result() == http::status::unauthorized);

    CHECK(HandlersOffMainThread == 0);
}

namespace
{
std::string PingPacket(uint16 opcode, uint32 requestId, std::string_view text)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Test::CreatePing(builder, builder.CreateString(text)));
    return WritePacket({ .Opcode = opcode, .RequestId = requestId }, { builder.GetBufferPointer(), builder.GetSize() });
}

// A packet sent, the one that came back: its header and its payload
std::pair<PacketHeader, std::string> ExchangePacket(WebSocketClient& client, std::string const& packet)
{
    client.binary(true);
    client.write(boost::asio::buffer(packet));
    boost::beast::flat_buffer buffer;
    client.read(buffer);
    REQUIRE_FALSE(client.got_text());
    std::string received = boost::beast::buffers_to_string(buffer.data());
    Optional<PacketHeader> header = ReadPacketHeader(received);
    REQUIRE(header);
    return { *header, received.substr(PacketHeaderSize) };
}
}

TEST_CASE("WorldWebSocket routes packets by opcode", "[server][websocket][packet]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WebSocketRouter router(sessions);
    router.AddPacket(1, "test.ping", &TestServerSession::HandlePing);
    router.AddPacket(2, "test.spam", &TestServerSession::HandlePing);

    AntiDosSettings antiDos;
    antiDos.Limits["test.spam"] = { .Window = 1h, .MaxCount = 1 };
    sessions.LoadSettings({}, antiDos);

    WorldHttpService service("test", sessions);
    service.WebSocketRoute("/ws", [&router](Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response,
        boost::asio::ip::address const& client)
    {
        return MakeUserWebSocket(router, request, response, client);
    });

    HandlersOffMainThread = 0;
    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    WebSocketClient client(Connect(clientIo, port));
    client.handshake("127.0.0.1", "/ws?user=bob");

    // The answer has the request's opcode and id
    auto [header, payload] = ExchangePacket(client, PingPacket(1, 77, "hi"));
    CHECK(header.Opcode == 1);
    CHECK(header.RequestId == 77);
    CHECK(flatbuffers::GetRoot<Test::Ping>(payload.data())->text()->str() == "hi user:bob");

    // JSON messages still work on the same connection
    client.text(true);
    CHECK(Exchange(client, R"({"type":"nope"})")["code"] == "unknown_type");

    auto errorOf = [](std::string const& payload)
    {
        flatbuffers::Verifier verifier(reinterpret_cast<uint8 const*>(payload.data()), payload.size());
        REQUIRE(verifier.VerifyBuffer<Rooftree::Packets::Error>(nullptr));
        return flatbuffers::GetRoot<Rooftree::Packets::Error>(payload.data());
    };

    auto [unknownHeader, unknown] = ExchangePacket(client, PingPacket(99, 5, "?"));
    CHECK(unknownHeader.Opcode == ErrorOpcode);
    CHECK(unknownHeader.RequestId == 5);
    CHECK(errorOf(unknown)->code()->str() == "unknown_opcode");
    CHECK(errorOf(unknown)->opcode() == 99);

    CHECK(ExchangePacket(client, PingPacket(2, 6, "once")).first.Opcode == 2);
    auto [limitedHeader, limited] = ExchangePacket(client, PingPacket(2, 7, "twice"));
    CHECK(limitedHeader.Opcode == ErrorOpcode);
    CHECK(limitedHeader.RequestId == 7);
    CHECK(errorOf(limited)->status() == 429);
    CHECK(errorOf(limited)->code()->str() == "rate_limited");

    // A payload that isn't a Ping: closed with 1007
    client.binary(true);
    client.write(boost::asio::buffer(WritePacket({ .Opcode = 1, .RequestId = 8 }, std::span<uint8 const>())));
    boost::beast::flat_buffer buffer;
    boost::system::error_code error;
    client.read(buffer, error);
    CHECK(error == boost::beast::websocket::error::closed);
    CHECK(client.reason().code == 1007);

    CHECK(HandlersOffMainThread == 0);
}

namespace
{
// Enough SMTP for curl: one mail per connection, no extensions, no TLS
class FakeSmtpServer
{
public:
    FakeSmtpServer() : _acceptor(_io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)), _thread([this] { Run(); }) { }

    ~FakeSmtpServer() { Join(); }

    // After the conversation, before reading what was received
    void Join()
    {
        if (_thread.joinable())
            _thread.join();
    }

    uint16 GetPort() const { return _acceptor.local_endpoint().port(); }

    std::string MailFrom;
    std::string RcptTo;
    std::string Data;

private:
    void Run()
    {
        tcp::socket socket(_io);
        _acceptor.accept(socket);
        timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
        setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        boost::asio::streambuf buffer;
        auto reply = [&](std::string_view line) { boost::asio::write(socket, boost::asio::buffer(line)); };
        auto readLine = [&]
        {
            boost::system::error_code error;
            boost::asio::read_until(socket, buffer, "\r\n", error);
            std::string line;
            std::istream stream(&buffer);
            std::getline(stream, line);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            return line;
        };

        reply("220 fake ESMTP\r\n");
        while (true)
        {
            std::string line = readLine();
            if (line.empty())
                return;

            if (line.starts_with("EHLO") || line.starts_with("HELO"))
                reply("250 fake\r\n");
            else if (line.starts_with("MAIL FROM:"))
            {
                MailFrom = line.substr(10);
                reply("250 OK\r\n");
            }
            else if (line.starts_with("RCPT TO:"))
            {
                RcptTo = line.substr(8);
                reply("250 OK\r\n");
            }
            else if (line == "DATA")
            {
                reply("354 go ahead\r\n");
                for (std::string dataLine = readLine(); dataLine != "."; dataLine = readLine())
                    Data += dataLine + "\n";
                reply("250 queued\r\n");
            }
            else if (line == "QUIT")
            {
                reply("221 bye\r\n");
                return;
            }
            else
                reply("502 not implemented\r\n");
        }
    }

    boost::asio::io_context _io;
    tcp::acceptor _acceptor;
    std::thread _thread;
};
}

TEST_CASE("Mailer sends over SMTP", "[server][mail]")
{
    FakeSmtpServer server;
    Mailer mailer({ .Url = "smtp://127.0.0.1:" + std::to_string(server.GetPort()), .From = "noreply@example.com", .RequireTls = false, .Timeout = 5s });

    std::string error;
    CHECK(mailer.Send({ .To = "user@example.com", .Subject = "Hello", .TextBody = "body" }, &error));
    CHECK(error.empty());

    server.Join();
    CHECK(server.MailFrom == "<noreply@example.com>");
    CHECK(server.RcptTo == "<user@example.com>");
    CHECK(server.Data.find("Subject: Hello\n") != std::string::npos);
    CHECK(server.Data.find("To: <user@example.com>\n") != std::string::npos);
    CHECK(server.Data.find("Ym9keQ==\n") != std::string::npos);    // "body"

    // Nothing listens there anymore
    Mailer unreachable({ .Url = "smtp://127.0.0.1:" + std::to_string(FindFreePort()), .From = "noreply@example.com", .RequireTls = false, .Timeout = 2s });
    CHECK_FALSE(unreachable.Send({ .To = "user@example.com", .Subject = "x", .TextBody = "x" }, &error));
    CHECK_FALSE(error.empty());
}

namespace
{
// A self-signed certificate for localhost and its key, as PEM files in a fresh temp directory
struct TestCertificate
{
    std::filesystem::path Directory;
    std::string CertificateFile;
    std::string KeyFile;

    TestCertificate()
    {
        std::random_device random;
        Directory = std::filesystem::temp_directory_path() / ("rooftree-tls-" + std::to_string(random()));
        std::filesystem::create_directories(Directory);
        CertificateFile = (Directory / "fullchain.pem").string();
        KeyFile = (Directory / "privkey.pem").string();
        Renew();
    }

    ~TestCertificate()
    {
        std::error_code error;
        std::filesystem::remove_all(Directory, error);
    }

    // A new key and certificate in the same files
    void Renew() const
    {
        EVP_PKEY* key = EVP_EC_gen("P-256");
        X509* certificate = X509_new();
        X509_set_version(certificate, 2);
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), std::random_device()() & 0x7FFFFFFF);
        X509_gmtime_adj(X509_getm_notBefore(certificate), 0);
        X509_gmtime_adj(X509_getm_notAfter(certificate), 3600);
        X509_set_pubkey(certificate, key);
        X509_NAME* name = X509_get_subject_name(certificate);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char const*>("localhost"), -1, -1, 0);
        X509_set_issuer_name(certificate, name);
        X509_sign(certificate, key, EVP_sha256());

        FILE* file = std::fopen(CertificateFile.c_str(), "w");
        PEM_write_X509(file, certificate);
        std::fclose(file);
        file = std::fopen(KeyFile.c_str(), "w");
        PEM_write_PrivateKey(file, key, nullptr, nullptr, 0, nullptr, nullptr);
        std::fclose(file);

        X509_free(certificate);
        EVP_PKEY_free(key);
    }
};

using TlsClient = boost::asio::ssl::stream<tcp::socket>;

std::unique_ptr<TlsClient> ConnectTls(boost::asio::io_context& io, boost::asio::ssl::context& clientContext, uint16 port)
{
    auto client = std::make_unique<TlsClient>(Connect(io, port), clientContext);
    client->handshake(boost::asio::ssl::stream_base::client);
    return client;
}

HttpResponse SendTls(TlsClient& client, std::string_view target)
{
    http::request<http::string_body> request(http::verb::get, target, 11);
    request.set(http::field::host, "localhost");
    http::write(client, request);
    boost::beast::flat_buffer buffer;
    HttpResponse response;
    http::read(client, buffer, response);
    return response;
}

// SHA-256 of the certificate the server presented
std::string PeerFingerprint(TlsClient& client)
{
    X509* certificate = SSL_get1_peer_certificate(client.native_handle());
    REQUIRE(certificate);
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    X509_digest(certificate, EVP_sha256(), digest, &length);
    X509_free(certificate);
    return std::string(reinterpret_cast<char*>(digest), length);
}
}

TEST_CASE("WorldHttpService over TLS", "[server][http][tls]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    TestCertificate certificate;
    WorldHttpService service("test.tls", sessions);
    service.SetSettings({ .SslCertificateFile = certificate.CertificateFile, .SslPrivateKeyFile = certificate.KeyFile });
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);
    std::string error;
    REQUIRE(service.IsSsl());
    REQUIRE(service.LoadSslContext(error));

    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    boost::asio::ssl::context clientContext(boost::asio::ssl::context::tls_client);
    clientContext.set_verify_mode(boost::asio::ssl::verify_none);

    std::unique_ptr<TlsClient> first = ConnectTls(clientIo, clientContext, port);
    HttpResponse hello = SendTls(*first, "/hello/tls");
    CHECK(hello.result() == http::status::ok);
    CHECK(JsonOf(hello)["hello"] == "tls");
    CHECK(SSL_version(first->native_handle()) >= TLS1_2_VERSION);

    SECTION("plain HTTP to the TLS port gets no answer")
    {
        tcp::socket plain = Connect(clientIo, port);
        boost::system::error_code writeError;
        http::request<http::string_body> request(http::verb::get, "/hello/plain", 11);
        http::write(plain, request, writeError);
        boost::beast::flat_buffer buffer;
        HttpResponse response;
        boost::system::error_code readError;
        http::read(plain, buffer, response, readError);
        CHECK(readError);
    }

    SECTION("TLS 1.1 is refused")
    {
        boost::asio::ssl::context oldContext(boost::asio::ssl::context::tls_client);
        oldContext.set_verify_mode(boost::asio::ssl::verify_none);
        SSL_CTX_set_max_proto_version(oldContext.native_handle(), TLS1_1_VERSION);
        TlsClient old(Connect(clientIo, port), oldContext);
        boost::system::error_code handshakeError;
        old.handshake(boost::asio::ssl::stream_base::client, handshakeError);
        CHECK(handshakeError);
    }

    SECTION("a renewed certificate: new connections get it, open ones keep theirs")
    {
        std::string oldFingerprint = PeerFingerprint(*first);
        certificate.Renew();
        REQUIRE(service.LoadSslContext(error));

        std::unique_ptr<TlsClient> second = ConnectTls(clientIo, clientContext, port);
        CHECK(PeerFingerprint(*second) != oldFingerprint);
        CHECK(SendTls(*second, "/hello/new").result() == http::status::ok);
        CHECK(SendTls(*first, "/hello/old").result() == http::status::ok);

        // A key that isn't the certificate's leaves the current context
        std::string newFingerprint = PeerFingerprint(*second);
        std::filesystem::path otherKey = certificate.Directory / "other.pem";
        std::filesystem::copy_file(certificate.KeyFile, otherKey);
        certificate.Renew();
        std::filesystem::copy_file(otherKey, certificate.KeyFile, std::filesystem::copy_options::overwrite_existing);
        CHECK_FALSE(service.LoadSslContext(error));
        CHECK_THAT(error, Catch::Matchers::ContainsSubstring("privkey.pem"));
        std::unique_ptr<TlsClient> third = ConnectTls(clientIo, clientContext, port);
        CHECK(PeerFingerprint(*third) == newFingerprint);
    }
}

TEST_CASE("WorldHttpService redirects plain HTTP to the TLS origin", "[server][http][tls]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WorldHttpService service("test.redirect", sessions);
    WorldHttpSettings settings{ .RedirectToOrigin = "https://example.com" };
    SECTION("everyone")
    {
    }
    SECTION("but the exempt")
    {
        settings.RedirectExempt = { boost::asio::ip::make_address("127.0.0.1") };
    }
    bool exempt = !settings.RedirectExempt.empty();
    service.SetSettings(settings);
    service.Route(http::verb::get, "/hello/{name}", "test.hello", &TestServerSession::HandleHello);

    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);
    HttpResponse hello = Send(socket, http::verb::get, "/hello/x?y=1");
    if (exempt)
        CHECK(hello.result() == http::status::ok);
    else
    {
        CHECK(hello.result() == http::status::moved_permanently);
        CHECK(hello[http::field::location] == "https://example.com/hello/x?y=1");
    }

    // ACME challenges are answered over plain HTTP: not redirected (here no route has them)
    tcp::socket acme = Connect(clientIo, port);
    CHECK(Send(acme, http::verb::get, "/.well-known/acme-challenge/token").result() == http::status::not_found);
}

TEST_CASE("WorldHttpService refuses certificates it can't use", "[server][http][tls]")
{
    TestServerSessionMgr sessions;
    WorldHttpService service("test.tls", sessions);
    service.SetSettings({ .SslCertificateFile = "/nonexistent/fullchain.pem", .SslPrivateKeyFile = "/nonexistent/privkey.pem" });
    std::string error;
    CHECK_FALSE(service.LoadSslContext(error));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("/nonexistent/fullchain.pem"));
}

TEST_CASE("WebSocketRoute over TLS, closed when silent", "[server][websocket][tls]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    TestServerSessionMgr sessions;
    WebSocketRouter router(sessions);
    router.Add("echo", &TestServerSession::HandleWsEcho);
    TestCertificate certificate;
    WorldHttpService service("test.wss", sessions);
    service.SetSettings({ .SslCertificateFile = certificate.CertificateFile, .SslPrivateKeyFile = certificate.KeyFile,
        .WebSocketIdleTimeout = 400ms });
    service.WebSocketRoute("/ws", [&router](Rooftree::Net::Http::Request const& request, Rooftree::Net::Http::Response& response,
        boost::asio::ip::address const& client)
    {
        return MakeUserWebSocket(router, request, response, client);
    });
    std::string error;
    REQUIRE(service.LoadSslContext(error));

    MainLoop mainLoop(sessions);
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 1));
    StopNetworkGuard<WorldHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    boost::asio::ssl::context clientContext(boost::asio::ssl::context::tls_client);
    clientContext.set_verify_mode(boost::asio::ssl::verify_none);
    boost::beast::websocket::stream<TlsClient> client(Connect(clientIo, port), clientContext);
    client.next_layer().handshake(boost::asio::ssl::stream_base::client);
    client.handshake("localhost", "/ws?user=bob");

    for (int i = 0; i < 3; ++i)
    {
        std::this_thread::sleep_for(200ms);
        client.write(boost::asio::buffer(std::string_view(R"({"type":"echo","n":1})")));
        boost::beast::flat_buffer buffer;
        client.read(buffer);
        CHECK(boost::json::parse(boost::beast::buffers_to_string(buffer.data())).as_object()["type"] == "echo");
    }

    // Nothing for longer than the idle timeout
    auto start = std::chrono::steady_clock::now();
    boost::beast::flat_buffer buffer;
    boost::system::error_code readError;
    client.read(buffer, readError);
    CHECK(readError);
    CHECK(std::chrono::steady_clock::now() - start < 3s);
}
