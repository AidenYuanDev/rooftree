/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "WorldHttpService.h"
#include "Errors.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "SessionMgr.h"
#include "SslContext.h"
#include "WebSocketConnection.h"
#include <boost/asio/post.hpp>
#include <algorithm>
#include <array>
#include <atomic>

namespace http = boost::beast::http;

namespace Rooftree::Server
{
namespace
{
std::atomic<uint64> RequestCount = 0;

bool IsRedirectExempt(WorldHttpSettings const& settings, boost::asio::ip::address address, std::string_view target)
{
    if (target.starts_with("/.well-known/acme-challenge/"))
        return true;

    if (address.is_v6() && address.to_v6().is_v4_mapped())
        address = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());
    return std::ranges::find(settings.RedirectExempt, address) != settings.RedirectExempt.end();
}
}

WorldHttpSocket::WorldHttpSocket(Net::IoContextTcpSocket&& socket, WorldHttpService* service, std::shared_ptr<boost::asio::ssl::context> sslContext)
    : BaseSocket(std::move(socket), sslContext.get()), _service(service), _sslContext(std::move(sslContext))
{
    ASSERT(_service);
}

void WorldHttpSocket::Start()
{
    WorldHttpSettings const& settings = _service->GetSettings();
    SetMaxRequestBodySize(settings.MaxRequestBodySize);
    SetTimeouts(settings.RequestTimeout, settings.KeepAliveTimeout);

    if (underlying_stream().IsSsl())
        Net::AcceptTls(*this, [](WorldHttpSocket& socket) { socket.StartHttp(); });
    else
        StartHttp();
}

Net::Http::RequestHandlerResult WorldHttpSocket::RequestHandler(Net::Http::RequestContext& context)
{
    RequestCount.fetch_add(1, std::memory_order_relaxed);

    // A blocked client's connection, opened before it was blocked: closed without an answer. Through a
    // proxy only the client is, the connection serves others too
    boost::asio::ip::address client = _service->GetClientAddress(context.request, GetRemoteIpAddress());
    if (_service->GetSessionMgr().GetAddressBlocklist()->IsBlocked(client, TimePoint::clock::now()))
    {
        if (client == GetRemoteIpAddress())
        {
            CloseSocket();
            return Net::Http::RequestHandlerResult::Async;
        }

        SetJsonError(context.response, http::status::too_many_requests, "rate_limited");
        context.response.set(http::field::retry_after, "60");
        return Net::Http::RequestHandlerResult::Handled;
    }

    WorldHttpSettings const& settings = _service->GetSettings();
    std::string_view target = Net::Http::ToStdStringView(context.request.target());
    if (!settings.RedirectToOrigin.empty() && !IsRedirectExempt(settings, GetRemoteIpAddress(), target))
    {
        // Only an origin-form target is appended, anything else could point elsewhere
        context.response.result(http::status::moved_permanently);
        context.response.set(http::field::location, settings.RedirectToOrigin + std::string(target.starts_with('/') ? target : "/"));
        context.response.keep_alive(false);
        return Net::Http::RequestHandlerResult::Handled;
    }

    return _service->HandleRequest(std::static_pointer_cast<WorldHttpSocket>(shared_from_this()), context);
}

WorldHttpRequest::WorldHttpRequest(std::shared_ptr<WorldHttpSocket> socket, Net::Http::RequestContext&& context, boost::asio::ip::address clientAddress)
    : _socket(std::move(socket)), _context(std::make_shared<Net::Http::RequestContext>(std::move(context))), _clientAddress(std::move(clientAddress))
{
}

WorldHttpRequest::~WorldHttpRequest()
{
    if (!_responded)
    {
        RT_LOG_ERROR("server.http", "{} {} was never answered, sending 500", Net::Http::ToStdStringView(_context->request.method_string()), GetTarget());
        RespondError(http::status::internal_server_error, "internal_error");
    }
}

Optional<std::string> WorldHttpRequest::GetQueryParameter(std::string_view name) const
{
    return Net::Http::GetQueryParameter(GetTarget(), name);
}

Optional<std::string_view> WorldHttpRequest::GetCookie(std::string_view name) const
{
    return Net::Http::GetCookie(_context->request, name);
}

void WorldHttpRequest::Respond()
{
    if (_responded.exchange(true))
        return;

    // Socket::QueuePacket isn't thread safe, the response is written on the connection's own thread
    boost::asio::post(_socket->underlying_stream().get_executor(), [socket = _socket, context = _context]
    {
        socket->SendResponse(*context);
    });
}

void WorldHttpRequest::RespondJson(http::status status, boost::json::value const& body)
{
    if (_responded)
        return;

    SetJsonResponse(_context->response, status, body);
    Respond();
}

void WorldHttpRequest::RespondStream(http::status status, std::shared_ptr<std::istream> stream, std::uint64_t length)
{
    if (_responded)
        return;

    _context->response.result(status);
    _context->response.body().clear();
    _context->responseStream = std::make_shared<Net::Http::ResponseStream>(Net::Http::ResponseStream{ .Stream = std::move(stream), .Length = length });
    Respond();
}

void WorldHttpRequest::RespondError(http::status status, std::string_view code, std::string_view message, boost::json::object fields)
{
    if (_responded)
        return;

    SetJsonError(_context->response, status, code, message, std::move(fields));
    Respond();
}

HttpMessage::HttpMessage(MessageType const& type, std::string sessionKey, WorldHttpRequestPtr request, WorldHttpHandler const& handler)
    : Message(type, std::move(sessionKey), request->GetClientAddress()), _request(std::move(request)), _handler(handler)
{
}

void HttpMessage::Process(Session& session)
{
    _handler(session, _request);
}

void HttpMessage::Reject(RejectReason reason)
{
    switch (reason)
    {
        case RejectReason::RateLimited:
            _request->GetResponse().set(http::field::retry_after, "60");
            _request->RespondError(http::status::too_many_requests, "rate_limited");
            break;
        case RejectReason::Blocked:   // blocked between the socket's check and the queue: as over a limit
            _request->GetResponse().set(http::field::retry_after, "60");
            _request->RespondError(http::status::too_many_requests, "rate_limited");
            break;
        case RejectReason::Overloaded:
            _request->GetResponse().set(http::field::retry_after, "1");
            _request->RespondError(http::status::service_unavailable, "overloaded");
            break;
        case RejectReason::SessionClosed:
        default:
            _request->RespondError(http::status::service_unavailable, "unavailable");
            break;
    }
}

WorldHttpService::WorldHttpService(std::string_view loggerSuffix, SessionMgr& sessionMgr) : HttpService(loggerSuffix), _sessionMgr(sessionMgr)
{
}

void WorldHttpService::SetSettings(WorldHttpSettings settings)
{
    _settings = std::move(settings);
    std::vector<boost::asio::ip::address> exempt = _settings.TrustedProxies;
    exempt.insert(exempt.end(), _settings.ConnectionLimitExempt.begin(), _settings.ConnectionLimitExempt.end());
    SetMaxConnectionsPerAddress(_settings.MaxConnectionsPerAddress, std::move(exempt));
    SetAddressBlocklist(_sessionMgr.GetAddressBlocklist());
}

void WorldHttpService::Spawn(Session& session, Rooftree::Task<void> handler)
{
    session.Spawn(std::move(handler));
}

void WorldHttpService::Route(http::verb method, std::string_view path, std::string_view messageType, WorldHttpHandler handler,
    Net::Http::RequestHandlerFlag flags)
{
    MessageType const& type = _sessionMgr.GetMessageTypes().Register(messageType);
    WorldHttpHandler const& storedHandler = _handlers.emplace_back(std::move(handler));
    _routes.push_back({ .Method = method, .Path = std::string(path), .MessageType = std::string(messageType) });

    RegisterHandler(method, path, [this, &type, &storedHandler](std::shared_ptr<WorldHttpSocket> socket, Net::Http::RequestContext& context)
    {
        boost::asio::ip::address client = GetClientAddress(context.request, socket->GetRemoteIpAddress());
        std::string key = GetSessionKey(context.request, client);
        WorldHttpRequestPtr request = std::make_shared<WorldHttpRequest>(std::move(socket), std::move(context), std::move(client));
        _sessionMgr.Enqueue(std::make_unique<HttpMessage>(type, std::move(key), std::move(request), storedHandler));
        return Net::Http::RequestHandlerResult::Async;
    }, flags);
}

void WorldHttpService::WebSocketRoute(std::string_view path, WebSocketFactory factory)
{
    RegisterHandler(http::verb::get, path, [this, factory = std::move(factory)](std::shared_ptr<WorldHttpSocket> socket, Net::Http::RequestContext& context)
    {
        // 400, or 426 for a version other than 13
        if (!Net::WebSocket::AcceptHandshake(context.request, context.response))
            return Net::Http::RequestHandlerResult::Handled;

        boost::asio::ip::address client = GetClientAddress(context.request, socket->GetRemoteIpAddress());
        std::shared_ptr<Net::WebSocket::Connection> connection = factory(context.request, context.response, client);
        if (!connection)
        {
            if (context.response.result() == http::status::switching_protocols)
                context.response.result(http::status::forbidden);
            context.response.erase(http::field::upgrade);
            context.response.erase(http::field::sec_websocket_accept);
            context.response.keep_alive(false);
            return Net::Http::RequestHandlerResult::Handled;
        }

        socket->UpgradeToWebSocket(std::move(connection), _settings.WebSocketIdleTimeout, _settings.WebSocketMaxWriteQueueSize);
        return Net::Http::RequestHandlerResult::Handled;
    });
}

boost::asio::ip::address WorldHttpService::GetClientAddress(Net::Http::Request const& request, boost::asio::ip::address const& peer) const
{
    if (!IsTrustedProxy(peer))
        return peer;

    // "client, proxy1, proxy2": walk back from the right while the entries are our own proxies
    std::string_view forwarded = Net::Http::ToStdStringView(request[http::field::x_forwarded_for]);
    boost::asio::ip::address client = peer;
    while (!forwarded.empty())
    {
        std::size_t comma = forwarded.rfind(',');
        std::string_view entry = comma == std::string_view::npos ? forwarded : forwarded.substr(comma + 1);
        forwarded = comma == std::string_view::npos ? std::string_view() : forwarded.substr(0, comma);

        std::size_t begin = entry.find_first_not_of(' ');
        std::size_t end = entry.find_last_not_of(' ');
        if (begin == std::string_view::npos)
            break;

        boost::system::error_code error;
        boost::asio::ip::address address = boost::asio::ip::make_address(entry.substr(begin, end - begin + 1), error);
        if (error)
            break;

        client = address;
        if (!IsTrustedProxy(address))
            break;
    }

    return client;
}

bool WorldHttpService::LoadSslContext(std::string& error)
{
    if (!IsSsl())
        return true;

    std::shared_ptr<boost::asio::ssl::context> context = Net::CreateServerSslContext(_settings.SslCertificateFile, _settings.SslPrivateKeyFile, error);
    if (!context)
        return false;

    std::scoped_lock lock(_sslContextLock);
    _sslContext = std::move(context);
    return true;
}

std::shared_ptr<WorldHttpSocket> WorldHttpService::CreateSocket(Net::IoContextTcpSocket&& socket)
{
    std::shared_ptr<boost::asio::ssl::context> sslContext;
    if (IsSsl())
    {
        std::scoped_lock lock(_sslContextLock);
        sslContext = _sslContext;
        // Without one, nothing would be safe to answer
        ASSERT(sslContext, "WorldHttpService: LoadSslContext must succeed before StartNetwork");
    }

    return std::make_shared<WorldHttpSocket>(std::move(socket), this, std::move(sslContext));
}

std::string WorldHttpService::GetSessionKey(Net::Http::Request const& /*request*/, boost::asio::ip::address const& clientAddress) const
{
    return AddressSessionKey(clientAddress);
}

uint64 WorldHttpService::GetRequestCount()
{
    return RequestCount.load(std::memory_order_relaxed);
}

std::string WorldHttpService::AddressSessionKey(boost::asio::ip::address const& clientAddress)
{
    boost::asio::ip::address address = clientAddress;
    if (address.is_v6() && address.to_v6().is_v4_mapped())
        address = boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());

    if (address.is_v4())
        return "ip:" + address.to_string();

    // One subscriber gets a whole /64 and can pick any address in it: one key per address would give it
    // as many sessions, and fresh AntiDOS counters, as it likes
    boost::asio::ip::address_v6::bytes_type bytes = address.to_v6().to_bytes();
    std::fill(bytes.begin() + 8, bytes.end(), 0);
    return "ip:" + boost::asio::ip::address_v6(bytes).to_string() + "/64";
}

bool WorldHttpService::IsTrustedProxy(boost::asio::ip::address const& address) const
{
    return std::ranges::find(_settings.TrustedProxies, address) != _settings.TrustedProxies.end();
}
}
