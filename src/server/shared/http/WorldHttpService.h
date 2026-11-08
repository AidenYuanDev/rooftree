/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * An HttpService whose requests run on the main thread, in the client's Session, like a game packet.
 * Per request:
 *
 *  network thread  the route is matched (DispatcherService), the request is wrapped in an
 *                  HttpMessage and enqueued on SessionMgr; the socket stops reading until the response
 *                  is out (BaseSocket answers one request at a time)
 *  main thread     SessionMgr hands it to the session of its key, AntiDOS checks its type, then the
 *                  route's handler runs with the session and a WorldHttpRequest
 *  any thread      the handler, or a database callback / worker it started, calls Respond(); the
 *                  response is posted back to the connection's network thread and written
 *
 * A request that is never answered gets a 500 when its last reference goes away, so a handler that
 * forgets to respond on some path doesn't leave the client hanging.
 *
 * Sessions are keyed by client address ("ip:...", an IPv6 one by its /64). Not by session cookie: the
 * network thread can't tell a real cookie from a made-up one, and one made-up cookie per request would
 * get a fresh session with fresh per-session limits every time. Behind a reverse proxy the client
 * address is the X-Forwarded-For entry right before the trusted proxies.
 *
 * Rooftree: a service is TLS when it has a certificate (WorldHttpSettings), its sockets are the same
 * type either way (OptionalSslStream). A plain one may redirect every request to the TLS origin, except
 * the ACME challenges and the requests of the host itself (scripts calling the API on 127.0.0.1).
 * WebSocketRoute upgrades a connection to WebSocket on the same port: wss:// with the same TLS, the
 * same origin as the pages.
 */

#ifndef ROOFTREE_WORLD_HTTP_SERVICE_H
#define ROOFTREE_WORLD_HTTP_SERVICE_H

#include "BaseHttpSocket.h"
#include "HttpService.h"
#include "JsonHttp.h"
#include "OptionalSslStream.h"
#include "ServerMessage.h"
#include "Task.h"
#include <concepts>
#include <deque>
#include <functional>
#include <mutex>
#include <vector>

namespace Rooftree::Net::WebSocket
{
class Connection;
}

namespace Rooftree::Server
{
class SessionMgr;
class WorldHttpService;

using WorldHttpStream = Net::OptionalSslStream<Net::Http::Impl::BoostBeastSocketWrapper>;

class WorldHttpSocket final : public Net::Http::BaseSocket<WorldHttpStream>
{
public:
    // service is never null: WorldHttpService::CreateSocket passes it. The default only satisfies
    // SocketMgr::CreateSocket, which is instantiated but never called. sslContext null: plain
    explicit WorldHttpSocket(Net::IoContextTcpSocket&& socket, WorldHttpService* service = nullptr,
        std::shared_ptr<boost::asio::ssl::context> sslContext = nullptr);

    void Start() override;

    Net::Http::RequestHandlerResult RequestHandler(Net::Http::RequestContext& context) override;

private:
    WorldHttpService* _service;
    std::shared_ptr<boost::asio::ssl::context> _sslContext;     // the stream's, kept as long as it lives
};

class WorldHttpRequest
{
public:
    WorldHttpRequest(std::shared_ptr<WorldHttpSocket> socket, Net::Http::RequestContext&& context, boost::asio::ip::address clientAddress);
    WorldHttpRequest(WorldHttpRequest const&) = delete;
    WorldHttpRequest& operator=(WorldHttpRequest const&) = delete;
    ~WorldHttpRequest();

    Net::Http::Request const& GetRequest() const { return _context->request; }
    std::string_view GetTarget() const { return Net::Http::ToStdStringView(_context->request.target()); }
    boost::beast::http::verb GetMethod() const { return _context->request.method(); }
    std::string_view GetPathParameter(std::string_view name) const { return _context->pathParameters.Get(name); }
    std::vector<std::pair<std::string, std::string>> const& GetPathParameters() const { return _context->pathParameters.GetAll(); }
    Optional<std::string> GetQueryParameter(std::string_view name) const;
    Optional<std::string_view> GetCookie(std::string_view name) const;
    std::string_view GetHeader(boost::beast::http::field field) const { return Net::Http::ToStdStringView(_context->request[field]); }
    std::string const& GetBody() const { return _context->request.body(); }
    boost::asio::ip::address const& GetClientAddress() const { return _clientAddress; }

    // Until Respond()
    Net::Http::Response& GetResponse() { return _context->response; }

    // Any thread, once; later calls do nothing. The response must not be touched afterwards
    void Respond();
    void RespondJson(boost::beast::http::status status, boost::json::value const& body);
    void RespondError(boost::beast::http::status status, std::string_view code, std::string_view message = {}, boost::json::object fields = {});
    // The body read from stream as it is sent (Net::Http::ResponseStream), headers set on GetResponse()
    // before: a file of any size without holding it in memory
    void RespondStream(boost::beast::http::status status, std::shared_ptr<std::istream> stream, std::uint64_t length);
    bool HasResponded() const { return _responded; }

private:
    std::shared_ptr<WorldHttpSocket> _socket;
    std::shared_ptr<Net::Http::RequestContext> _context;
    boost::asio::ip::address _clientAddress;
    std::atomic<bool> _responded = false;
};

using WorldHttpRequestPtr = std::shared_ptr<WorldHttpRequest>;
using WorldHttpHandler = std::function<void(Session& session, WorldHttpRequestPtr const& request)>;

class HttpMessage final : public Message
{
public:
    HttpMessage(MessageType const& type, std::string sessionKey, WorldHttpRequestPtr request, WorldHttpHandler const& handler);

    WorldHttpRequestPtr const& GetRequest() const { return _request; }

    void Process(Session& session) override;
    void Reject(RejectReason reason) override;

private:
    WorldHttpRequestPtr _request;
    WorldHttpHandler const& _handler;   // owned by the route, which outlives every request
};

struct WorldHttpSettings
{
    // Peers whose X-Forwarded-For is believed (a reverse proxy on the same host: 127.0.0.1). They are
    // also exempt from MaxConnectionsPerAddress, their connections are many clients'
    std::vector<boost::asio::ip::address> TrustedProxies;
    std::uint64_t MaxRequestBodySize = 1024 * 1024;
    // BaseSocket::SetTimeouts, 0 disables
    Milliseconds RequestTimeout = 0ms;
    Milliseconds KeepAliveTimeout = 0ms;
    // Open at once per peer address, 0 is no limit. TrustedProxies and ConnectionLimitExempt (the host
    // itself, whose scripts may open many) aren't counted
    uint32 MaxConnectionsPerAddress = 0;
    std::vector<boost::asio::ip::address> ConnectionLimitExempt;
    // Both set: TLS (CreateServerSslContext). The files are read by LoadSslContext
    std::string SslCertificateFile;
    std::string SslPrivateKeyFile;
    // Set ("https://example.com"): a plain service answers every request with a 301 there, except
    // /.well-known/acme-challenge/ and peers in RedirectExempt (the host itself)
    std::string RedirectToOrigin;
    std::vector<boost::asio::ip::address> RedirectExempt;
    // A WebSocket connection without anything from the client this long is closed (0: never), and once
    // more bytes than MaxWriteQueueSize wait to be sent to it (0: no limit)
    Milliseconds WebSocketIdleTimeout = 0ms;
    std::size_t WebSocketMaxWriteQueueSize = 0;
};

// Network thread, for a valid handshake request: the connection, or null to refuse it after setting the
// response's status (403 if left at 101). clientAddress is the client in front of the trusted proxies
using WebSocketFactory = std::function<std::shared_ptr<Net::WebSocket::Connection>(Net::Http::Request const& request,
    Net::Http::Response& response, boost::asio::ip::address const& clientAddress)>;

class WorldHttpService : public Net::Http::HttpService<WorldHttpSocket>
{
public:
    WorldHttpService(std::string_view loggerSuffix, SessionMgr& sessionMgr);

    // Before StartNetwork
    void SetSettings(WorldHttpSettings settings);
    WorldHttpSettings const& GetSettings() const { return _settings; }

    // Reads the certificate and key of the settings, before StartNetwork and again when they were
    // renewed (any thread). New connections get the new context, open ones keep theirs. False, with
    // error set and the current context kept, if they can't be used. True without TLS settings
    bool LoadSslContext(std::string& error);
    bool IsSsl() const { return !_settings.SslCertificateFile.empty(); }

    // Before StartNetwork. messageType names the route for AntiDOS limits and logs
    void Route(boost::beast::http::verb method, std::string_view path, std::string_view messageType, WorldHttpHandler handler,
        Net::Http::RequestHandlerFlag flags = Net::Http::RequestHandlerFlag::None);

    // The routes in the order they were added: what a console can call (Blog's "api routes")
    struct RouteInfo
    {
        boost::beast::http::verb Method;
        std::string Path;
        std::string MessageType;
    };
    std::vector<RouteInfo> const& GetRoutes() const { return _routes; }

    // Before StartNetwork: a GET of path with a WebSocket handshake upgrades the connection to the
    // connection factory makes, that the socket runs from then on. Other requests of path get 400
    void WebSocketRoute(std::string_view path, WebSocketFactory factory);

    // A member function of the app's session class
    template<std::derived_from<Session> SessionImpl>
    void Route(boost::beast::http::verb method, std::string_view path, std::string_view messageType,
        void (SessionImpl::*handler)(WorldHttpRequestPtr const&), Net::Http::RequestHandlerFlag flags = Net::Http::RequestHandlerFlag::None)
    {
        Route(method, path, messageType, [handler](Session& session, WorldHttpRequestPtr const& request)
        {
            (static_cast<SessionImpl&>(session).*handler)(request);
        }, flags);
    }

    // One that is a coroutine: Session::Spawn runs it
    template<std::derived_from<Session> SessionImpl>
    void Route(boost::beast::http::verb method, std::string_view path, std::string_view messageType,
        Rooftree::Task<void> (SessionImpl::*handler)(WorldHttpRequestPtr), Net::Http::RequestHandlerFlag flags = Net::Http::RequestHandlerFlag::None)
    {
        Route(method, path, messageType, [handler](Session& session, WorldHttpRequestPtr const& request)
        {
            Spawn(session, (static_cast<SessionImpl&>(session).*handler)(request));
        }, flags);
    }

    SessionMgr& GetSessionMgr() const { return _sessionMgr; }

    // Network thread: the peer, or the client in front of trusted proxies
    boost::asio::ip::address GetClientAddress(Net::Http::Request const& request, boost::asio::ip::address const& peer) const;

    // "ip:203.0.113.7", IPv4-mapped IPv6 written as IPv4, IPv6 by its /64 ("ip:2001:db8:1:2::/64"): the
    // default session key, also for other front ends (a WebSocket of the same app) that want the same
    // session as the address's HTTP requests
    static std::string AddressSessionKey(boost::asio::ip::address const& clientAddress);

    // Any thread: the requests all services of the process were sent since the start, redirects and
    // static files included (a status page's requests per minute)
    static uint64 GetRequestCount();

protected:
    static void Spawn(Session& session, Rooftree::Task<void> handler);

    std::shared_ptr<WorldHttpSocket> CreateSocket(Net::IoContextTcpSocket&& socket) override;

    // Network thread: the key of the session that runs a request
    virtual std::string GetSessionKey(Net::Http::Request const& request, boost::asio::ip::address const& clientAddress) const;

private:
    bool IsTrustedProxy(boost::asio::ip::address const& address) const;

    SessionMgr& _sessionMgr;
    WorldHttpSettings _settings;
    mutable std::mutex _sslContextLock;
    std::shared_ptr<boost::asio::ssl::context> _sslContext;
    std::deque<WorldHttpHandler> _handlers;     // stable, HttpMessages reference them
    std::vector<RouteInfo> _routes;
};
}

#endif // ROOFTREE_WORLD_HTTP_SERVICE_H
