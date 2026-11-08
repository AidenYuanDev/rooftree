/*
 * websocket_server: the net/websocket module, a WebSocket echo server for browsers.
 *
 * Same structure as echo_server (SocketMgr + NetworkThreads), only the socket type changes:
 * WebSocket::BaseSocket does the opening handshake and the framing, OnMessage gets whole messages.
 *
 * Try:  ./websocket_server  then open websocket.html (next to the binary) in a browser, or
 *       websocat ws://127.0.0.1:7001/echo
 * Connections to any path other than /echo are refused with 404 in OnHandshake.
 */

#include "Config.h"
#include "IoContext.h"
#include "Log.h"
#include "SocketMgr.h"
#include "WebSocketSocket.h"
#include <boost/asio/signal_set.hpp>

using namespace Rooftree::Net;

class EchoWebSocket final : public WebSocket::BaseSocket<>
{
public:
    using BaseSocket::BaseSocket;

protected:
    bool OnHandshake(Http::Request const& request, Http::Response& response) override
    {
        if (request.target() == "/echo")
            return true;

        response.result(boost::beast::http::status::not_found);
        return false;
    }

    void OnOpen() override
    {
        RT_LOG_INFO("websocket", "{} connected", GetClientInfo());
        SendText("welcome, everything you send comes back");
    }

    void OnMessage(WebSocket::Opcode opcode, std::string&& payload) override
    {
        if (opcode == WebSocket::Opcode::Binary)
            SendBinary(payload);
        else
            SendText(payload);
    }

    void OnClose() override
    {
        RT_LOG_INFO("websocket", "{} disconnected", GetClientInfo());
    }
};

class EchoWebSocketNetworkThread final : public NetworkThread<EchoWebSocket, EchoWebSocketNetworkThread> { };

class EchoWebSocketServer;

struct EchoWebSocketServerTraits
{
    using Self = EchoWebSocketServer;
    using SocketType = EchoWebSocket;
    using ThreadType = EchoWebSocketNetworkThread;
};

class EchoWebSocketServer final : public SocketMgr<EchoWebSocketServerTraits> { };

int main(int argc, char** argv)
{
    std::string configFile = argc > 1 ? argv[1] : "rooftree.yaml";
    std::string configError;
    if (!sConfigMgr->LoadFile(configFile, configError))
        fmt::print(stderr, "Could not load {} ({}), using defaults\n", configFile, configError);

    sLog->LoadFromConfig();

    Rooftree::Asio::IoContext ioContext;

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    std::string bindIp = config->Get<std::string>("WebSocket.BindIP", "0.0.0.0");
    uint16 port = config->Get<uint16>("WebSocket.Port", 7001);
    int threads = config->Get<int>("WebSocket.NetworkThreads", 1);

    EchoWebSocketServer server;
    if (!server.StartNetwork(ioContext, bindIp, port, threads))
        return 1;

    RT_LOG_INFO("websocket", "Listening on ws://{}:{}/echo, Ctrl+C to stop", bindIp, port);

    boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor> signals(ioContext, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code const&, int) { ioContext.stop(); });

    ioContext.run();

    server.StopNetwork();
    return 0;
}
