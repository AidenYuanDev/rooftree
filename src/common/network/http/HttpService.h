/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * An HTTP server: a SocketMgr (SocketMgr.h) of SessionImpl connections and the routes their requests
 * go to. A SessionImpl's RequestHandler passes the request to HandleRequest, which sets the common
 * headers and calls the route's handler:
 *
 *   service.RegisterHandler(verb::get, "/users/{id}", [](std::shared_ptr<MySocket> socket, RequestContext& context)
 *   {
 *       context.response.body() = ...;
 *       return RequestHandlerResult::Handled;
 *   });
 *
 * A path without a route is 404 (or the fallback's, static files), one with routes of other methods
 * 405 with Allow, one that can't be decoded 400. Routes are added before the network starts.
 */

#ifndef ROOFTREE_HTTP_SERVICE_H
#define ROOFTREE_HTTP_SERVICE_H

#include "Concepts.h"
#include "HttpCommon.h"
#include "Optional.h"
#include "SocketMgr.h"
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace Rooftree::Net::Http
{
class AbstractSocket;

using HandlerFunction = std::function<RequestHandlerResult(std::shared_ptr<AbstractSocket> socket, RequestContext& context)>;

struct RequestHandler
{
    HandlerFunction Func;
    RequestHandlerFlag Flags = RequestHandlerFlag::None;
};

class DispatcherService
{
public:
    // Its log is server.http.dispatcher.<loggerSuffix>
    explicit DispatcherService(std::string_view loggerSuffix) : _logger("server.http.dispatcher." + std::string(loggerSuffix)) { }

    // A connection's network thread
    RequestHandlerResult HandleRequest(std::shared_ptr<AbstractSocket> socket, RequestContext& context);

    // What answers a path no route has, whatever its method, instead of a 404: static files under
    // paths of any depth, which {name} segments can't express
    void SetFallbackHandler(HandlerFunction handler) { _fallback = RequestHandler{ .Func = std::move(handler) }; }
    // Set on every response a handler answers (security headers); a handler may change them
    void SetDefaultHeaders(std::vector<std::pair<std::string, std::string>> headers) { _defaultHeaders = std::move(headers); }

protected:
    // A second route of the same method and path aborts
    void RegisterHandler(boost::beast::http::verb method, std::string_view path, HandlerFunction handler,
        RequestHandlerFlag flags = RequestHandlerFlag::None);

private:
    Router<RequestHandler> _router;
    Optional<RequestHandler> _fallback;
    std::vector<std::pair<std::string, std::string>> _defaultHeaders;
    std::string _logger;
};

template<typename Callable, typename SessionImpl>
concept HttpRequestHandler = invocable_r<Callable, RequestHandlerResult, std::shared_ptr<SessionImpl>, RequestContext&>;

template<typename SessionImpl>
class HttpService;

template<typename SessionImpl>
class HttpNetworkThread final : public NetworkThread<SessionImpl, HttpNetworkThread<SessionImpl>>
{
};

template<typename SessionImpl>
struct HttpServiceTraits
{
    using Self = HttpService<SessionImpl>;
    using SocketType = SessionImpl;
    using ThreadType = HttpNetworkThread<SessionImpl>;
};

template<typename SessionImpl>
class HttpService : public SocketMgr<HttpServiceTraits<SessionImpl>>, public DispatcherService
{
public:
    explicit HttpService(std::string_view loggerSuffix) : DispatcherService(loggerSuffix) { }

    bool StartNetwork(Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int32 threadCount = 1) override
    {
        return SocketMgr<HttpServiceTraits<SessionImpl>>::StartNetwork(ioContext, bindIp, port, threadCount);
    }

    using DispatcherService::RegisterHandler;

    // A handler of SessionImpl's connections
    template<HttpRequestHandler<SessionImpl> Callable>
    void RegisterHandler(boost::beast::http::verb method, std::string_view path, Callable handler, RequestHandlerFlag flags = RequestHandlerFlag::None)
    {
        DispatcherService::RegisterHandler(method, path, [handler = std::move(handler)](std::shared_ptr<AbstractSocket> socket, RequestContext& context)
        {
            return handler(std::static_pointer_cast<SessionImpl>(std::move(socket)), context);
        }, flags);
    }
};
}

#endif // ROOFTREE_HTTP_SERVICE_H
