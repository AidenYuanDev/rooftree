/*
 * http_server: the net/http module (Boost.Beast on top of Rooftree::Net::Socket).
 *
 * Try:  ./http_server  then
 *       curl -i http://127.0.0.1:8080/hello
 *       curl -i -X POST -d 'ping' http://127.0.0.1:8080/echo
 */

#include "Config.h"
#include "HttpService.h"
#include "HttpSocket.h"
#include "IoContext.h"
#include "Log.h"
#include <boost/asio/signal_set.hpp>

using namespace Rooftree::Net;

class HelloSession;

class HelloService final : public Http::HttpService<HelloSession>
{
public:
    HelloService() : HttpService("hello") { }

    static HelloService& Instance()
    {
        static HelloService instance;
        return instance;
    }

    void RegisterHandlers();
};

#define sHelloService HelloService::Instance()

// Http::Socket's Start() reads requests and hands each to RequestHandler
class HelloSession final : public Http::Socket
{
public:
    using Http::Socket::Socket;

    Http::RequestHandlerResult RequestHandler(Http::RequestContext& context) override
    {
        return sHelloService.HandleRequest(std::static_pointer_cast<HelloSession>(shared_from_this()), context);
    }
};

void HelloService::RegisterHandlers()
{
    RegisterHandler(boost::beast::http::verb::get, "/hello", [](std::shared_ptr<HelloSession> session, Http::RequestContext& context)
    {
        context.response.result(boost::beast::http::status::ok);
        context.response.set(boost::beast::http::field::content_type, "text/plain");
        context.response.body() = Rooftree::StringFormat("hello, {}\n", session->GetClientInfo());
        return Http::RequestHandlerResult::Handled;
    });

    RegisterHandler(boost::beast::http::verb::post, "/echo", [](std::shared_ptr<HelloSession> /*session*/, Http::RequestContext& context)
    {
        context.response.result(boost::beast::http::status::ok);
        context.response.set(boost::beast::http::field::content_type, "text/plain");
        context.response.body() = context.request.body();
        return Http::RequestHandlerResult::Handled;
    });
}

int main(int argc, char** argv)
{
    std::string configFile = argc > 1 ? argv[1] : "rooftree.yaml";
    std::string configError;
    if (!sConfigMgr->LoadFile(configFile, configError))
        fmt::print(stderr, "Could not load {} ({}), using defaults\n", configFile, configError);

    sLog->LoadFromConfig();

    Rooftree::Asio::IoContext ioContext;

    std::shared_ptr<Config const> config = sConfigMgr->GetConfig();
    std::string bindIp = config->Get<std::string>("Http.BindIP", "0.0.0.0");
    uint16 port = config->Get<uint16>("Http.Port", 8080);

    sHelloService.RegisterHandlers();
    if (!sHelloService.StartNetwork(ioContext, bindIp, port, config->Get<int>("Http.NetworkThreads", 1)))
        return 1;

    RT_LOG_INFO("server.http", "Listening on http://{}:{}, Ctrl+C to stop", bindIp, port);

    boost::asio::basic_signal_set<Rooftree::Asio::IoContextExecutor> signals(ioContext, SIGINT, SIGTERM);
    signals.async_wait([&](boost::system::error_code const&, int) { ioContext.stop(); });

    // Runs the acceptor and SessionService's inactive-session cleanup timer
    ioContext.run();

    sHelloService.StopNetwork();
    return 0;
}
