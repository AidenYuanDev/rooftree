/*
 * echo_server: the smallest possible user of the net module.
 *
 * Shows the same structure WorldSocketMgr / bnetserver's SessionManager use:
 *   Socket<>         - one connection, ReadHandler() is called with new bytes
 *   NetworkThread    - owns an io_context + the sockets assigned to it, calls Socket::Update()
 *   SocketMgr        - acceptor + N NetworkThreads, picks the thread with fewest connections
 *   Start()          - what a connection does first: a TLS handshake (AcceptTls), then reading
 *
 * Try:  ./echo_server  then  nc 127.0.0.1 7000
 */

#include "Config.h"
#include "IoContext.h"
#include "IpAddress.h"
#include "Log.h"
#include "SocketMgr.h"
#include <boost/asio/signal_set.hpp>

using namespace Rooftree::Net;

class EchoSocket final : public Socket<>
{
public:
    using Socket::Socket;

    void Start() override
    {
        RT_LOG_INFO("echo", "{}:{} connected", GetRemoteIpAddress(), GetRemotePort());

        // A TLS server would call AcceptTls(*this, ...) here and read once it is done (SslStream.h)
        StartReading();
    }

    // Called after each read, with the new bytes at the end of GetReadBuffer()
    SocketReadCallbackResult ReadHandler() override
    {
        MessageBuffer& packet = GetReadBuffer();

        MessageBuffer reply(packet.GetActiveSize());
        reply.Write(packet.GetReadPointer(), packet.GetActiveSize());
        packet.ReadCompleted(packet.GetActiveSize());

        // Not written immediately: NetworkThread calls Update() which flushes the write queue
        QueuePacket(std::move(reply));
        return SocketReadCallbackResult::KeepReading;
    }

protected:
    void OnClose() override
    {
        RT_LOG_INFO("echo", "{}:{} disconnected", GetRemoteIpAddress(), GetRemotePort());
    }
};

class EchoNetworkThread final : public NetworkThread<EchoSocket, EchoNetworkThread> { };

class EchoServer;

struct EchoServerTraits
{
    using Self = EchoServer;
    using SocketType = EchoSocket;
    using ThreadType = EchoNetworkThread;
};

class EchoServer final : public SocketMgr<EchoServerTraits> { };

int main(int argc, char** argv)
{
    std::string configFile = argc > 1 ? argv[1] : "rooftree.yaml";
    std::string configError;
    if (!sConfigMgr->LoadFile(configFile, configError))
        fmt::print(stderr, "Could not load {} ({}), using defaults\n", configFile, configError);

    sLog->LoadFromConfig();

    Rooftree::Asio::IoContext ioContext;

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    std::string bindIp = config->Get<std::string>("Echo.BindIP", "0.0.0.0");
    uint16 port = config->Get<uint16>("Echo.Port", 7000);
    int threads = config->Get<int>("Echo.NetworkThreads", 2);

    EchoServer server;
    if (!server.StartNetwork(ioContext, bindIp, port, threads))
        return 1;

    RT_LOG_INFO("echo", "Listening on {}:{} with {} network threads, Ctrl+C to stop", bindIp, port, threads);

    boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor> signals(ioContext, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code const&, int) { ioContext.stop(); });

    // The main io_context only runs the acceptor and the signal handler; connections live on the NetworkThreads
    ioContext.run();

    server.StopNetwork();
    RT_LOG_INFO("echo", "Stopped");
    return 0;
}
