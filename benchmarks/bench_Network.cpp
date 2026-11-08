/*
 * Loopback latency and throughput of rooftree-network: a SocketMgr echo server and an HttpService, driven
 * by blocking boost::asio clients. Results include the client side and the kernel's loopback, so
 * compare them between builds on the same machine rather than reading them as absolute numbers.
 *
 * Outside of IOCP, Socket::QueuePacket only queues the reply, NetworkThread::Update() starts the write
 * on its 1 ms timer. Request/response latency therefore has a ~1 ms floor per round trip.
 */

#include "rooftree_benchmark.h"
#include <random>

#include "HttpService.h"
#include "HttpSocket.h"
#include "IoContext.h"
#include "SocketMgr.h"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/write.hpp>
#include <sys/socket.h>

using boost::asio::ip::tcp;

namespace
{
// Same helpers as tests/test_Network.cpp
uint16 FindFreePort()
{
    boost::asio::io_context io;
    tcp::acceptor acceptor(io, tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0));
    return acceptor.local_endpoint().port();
}

tcp::socket Connect(boost::asio::io_context& io, uint16 port)
{
    tcp::socket socket(io);
    socket.connect(tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), port));
    socket.set_option(tcp::no_delay(true));
    timeval timeout{ .tv_sec = 5, .tv_usec = 0 };
    setsockopt(socket.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return socket;
}

class ServerContext
{
public:
    ServerContext() : _guard(boost::asio::make_work_guard(_ioContext)), _thread([this] { _ioContext.run(); }) { }

    ~ServerContext() { Stop(); }

    void Stop()
    {
        if (!_thread.joinable())
            return;

        _guard.reset();
        _ioContext.stop();
        _thread.join();
    }

    Rooftree::Asio::IoContext& Get() { return _ioContext; }

private:
    Rooftree::Asio::IoContext _ioContext;
    boost::asio::executor_work_guard<Rooftree::Asio::IoContextExecutor> _guard;
    std::thread _thread;
};

template <typename Server>
struct StopNetworkGuard
{
    ServerContext& context;
    Server& server;
    ~StopNetworkGuard()
    {
        context.Stop();
        server.StopNetwork();
    }
};

class EchoSocket final : public Rooftree::Net::Socket<>
{
public:
    using Socket::Socket;

    // SocketMgr turns Nagle off
    void Start() override { StartReading(); }

    Rooftree::Net::SocketReadCallbackResult ReadHandler() override
    {
        MessageBuffer& packet = GetReadBuffer();
        MessageBuffer reply(packet.GetActiveSize());
        reply.Write(packet.GetReadPointer(), packet.GetActiveSize());
        packet.ReadCompleted(packet.GetActiveSize());
        QueuePacket(std::move(reply));
        return Rooftree::Net::SocketReadCallbackResult::KeepReading;
    }
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

std::size_t EchoRoundTrip(tcp::socket& socket, std::string const& message, std::string& reply)
{
    boost::asio::write(socket, boost::asio::buffer(message));
    reply.resize(message.size());
    return boost::asio::read(socket, boost::asio::buffer(reply));
}
}

TEST_CASE("SocketMgr echo over loopback", "[network]")
{
    int networkThreads = std::min(4, std::max(1, int(std::thread::hardware_concurrency()) / 2));
    ServerContext context;
    uint16 port = FindFreePort();
    EchoServer server;
    REQUIRE(server.StartNetwork(context.Get(), "127.0.0.1", port, networkThreads));
    StopNetworkGuard<EchoServer> stopGuard{ context, server };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);
    std::string reply;

    for (std::size_t size : { 64, 4096 })
    {
        std::string message(size, 'x');
        BENCHMARK(Bench::Name("round trip", int(size), "bytes")) { return EchoRoundTrip(socket, message, reply); };
    }

    // Writes from another thread, the server echoes while we are still sending
    std::string bulk(1 << 20, 'y');
    BENCHMARK("stream 1 MiB through the echo")
    {
        std::thread writer([&] { boost::asio::write(socket, boost::asio::buffer(bulk)); });
        reply.resize(bulk.size());
        std::size_t read = boost::asio::read(socket, boost::asio::buffer(reply));
        writer.join();
        return read;
    };

    BENCHMARK("connect, 64 byte round trip, close")
    {
        tcp::socket shortLived = Connect(clientIo, port);
        std::string shortReply;
        return EchoRoundTrip(shortLived, std::string(64, 'z'), shortReply);
    };

    // Connections stay open between runs, each run does 100 round trips per client
    for (int clients : { 4, 16, 64 })
    {
        std::vector<tcp::socket> sockets;
        for (int i = 0; i < clients; ++i)
            sockets.push_back(Connect(clientIo, port));

        BENCHMARK(Bench::Name("100 round trips per client", clients, "clients"))
        {
            Bench::RunThreads(clients, [&](int index)
            {
                std::string message(64, char('a' + index % 26));
                std::string clientReply;
                for (int i = 0; i < 100; ++i)
                    EchoRoundTrip(sockets[index], message, clientReply);
            });
            return clients;
        };
    }
}

namespace
{
class BenchHttpSession;

class BenchHttpService final : public Rooftree::Net::Http::HttpService<BenchHttpSession>
{
public:
    BenchHttpService() : HttpService("bench")
    {
        RegisterHandler(boost::beast::http::verb::get, "/hello", [](std::shared_ptr<BenchHttpSession> /*session*/, Rooftree::Net::Http::RequestContext& context)
        {
            context.response.result(boost::beast::http::status::ok);
            context.response.set(boost::beast::http::field::content_type, "text/plain");
            context.response.body() = "hello";
            return Rooftree::Net::Http::RequestHandlerResult::Handled;
        });

        RegisterHandler(boost::beast::http::verb::post, "/echo", [](std::shared_ptr<BenchHttpSession> /*session*/, Rooftree::Net::Http::RequestContext& context)
        {
            context.response.result(boost::beast::http::status::ok);
            context.response.body() = context.request.body();
            return Rooftree::Net::Http::RequestHandlerResult::Handled;
        });
    }

    static BenchHttpService* Current;
};

BenchHttpService* BenchHttpService::Current = nullptr;

class BenchHttpSession final : public Rooftree::Net::Http::Socket
{
public:
    using Socket::Socket;

    Rooftree::Net::Http::RequestHandlerResult RequestHandler(Rooftree::Net::Http::RequestContext& context) override
    {
        return BenchHttpService::Current->HandleRequest(std::static_pointer_cast<BenchHttpSession>(shared_from_this()), context);
    }
};

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

TEST_CASE("HttpService over loopback", "[network][http]")
{
    ServerContext context;
    uint16 port = FindFreePort();
    BenchHttpService service;
    BenchHttpService::Current = &service;
    REQUIRE(service.StartNetwork(context.Get(), "127.0.0.1", port, 2));
    StopNetworkGuard<BenchHttpService> stopGuard{ context, service };

    boost::asio::io_context clientIo;
    tcp::socket socket = Connect(clientIo, port);

    BENCHMARK("GET /hello (keep-alive)")
    {
        return SendRequest(socket, boost::beast::http::verb::get, "/hello").body().size();
    };

    std::string body(1024, 'p');
    BENCHMARK("POST /echo 1 KiB (keep-alive)")
    {
        return SendRequest(socket, boost::beast::http::verb::post, "/echo", body).body().size();
    };

    BENCHMARK("GET /missing (404)")
    {
        return int(SendRequest(socket, boost::beast::http::verb::get, "/missing").result());
    };

    std::mt19937 phase(7);
    BENCHMARK("GET /hello on a new connection, random phase")
    {
        // Wait 0 to 1 ms first (measured, the same waits on both sides): the request lands anywhere
        // between two network updates, not right after one
        auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(phase() % 1000);
        while (std::chrono::steady_clock::now() < until)
            ;
        tcp::socket shortLived = Connect(clientIo, port);
        return SendRequest(shortLived, boost::beast::http::verb::get, "/hello").body().size();
    };

    BENCHMARK("GET /hello on a new connection")
    {
        tcp::socket shortLived = Connect(clientIo, port);
        return SendRequest(shortLived, boost::beast::http::verb::get, "/hello").body().size();
    };

    for (int clients : { 4, 16 })
    {
        std::vector<tcp::socket> sockets;
        for (int i = 0; i < clients; ++i)
            sockets.push_back(Connect(clientIo, port));

        BENCHMARK(Bench::Name("100 GET /hello per client", clients, "clients"))
        {
            Bench::RunThreads(clients, [&](int index)
            {
                for (int i = 0; i < 100; ++i)
                    SendRequest(sockets[index], boost::beast::http::verb::get, "/hello");
            });
            return clients;
        };
    }
}
