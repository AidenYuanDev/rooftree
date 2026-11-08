/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * An HTTP/1.1 connection on a Socket (Socket.h), one request after the other:
 *
 *   read      bytes go to Beast's parser until a request is whole, then to RequestHandler (the derived
 *             class's, which usually asks an HttpService for the route). A request answered later
 *             (Async) pauses reading, so a client can't pile requests up; SendResponse picks it up again
 *   respond   SendResponse serializes the response into the write queue, or its head and then a body
 *             streamed from a file a piece at a time. Connection: close closes once it is written
 *   timeouts  SetTimeouts: until a request's headers are in, between two reads of its body, between a
 *             response and the next request. None while a handler works
 *   upgrade   a handler answering a WebSocket handshake with 101 hands the connection over to a
 *             WebSocket::Connection, which reads and writes frames from then on
 *
 * A malformed request is answered 400, one over SetMaxRequestBodySize 413, and the connection closes.
 */

#ifndef ROOFTREE_BASE_HTTP_SOCKET_H
#define ROOFTREE_BASE_HTTP_SOCKET_H

#include "HttpCommon.h"
#include "Optional.h"
#include "Socket.h"
#include "WebSocketConnection.h"
#include <boost/beast/core/basic_stream.hpp>
#include <boost/beast/http/parser.hpp>
#include <boost/beast/http/string_body.hpp>

namespace Rooftree::Net::Http
{
using IoContextHttpSocket = boost::beast::basic_stream<boost::asio::ip::tcp, Asio::IoContextExecutor, boost::beast::unlimited_rate_policy>;

namespace Impl
{
// Beast's TCP stream with what Socket asks of a stream
class BoostBeastSocketWrapper : public IoContextHttpSocket
{
public:
    using IoContextHttpSocket::basic_stream;

    bool is_open() const { return socket().is_open(); }
    void close(boost::system::error_code& /*error*/) { IoContextHttpSocket::close(); }
    void cancel(boost::system::error_code& error) { socket().cancel(error); }
    void shutdown(boost::asio::socket_base::shutdown_type what, boost::system::error_code& error) { socket().shutdown(what, error); }

    template<typename Token>
    auto async_wait(boost::asio::socket_base::wait_type type, Token&& token) { return socket().async_wait(type, std::forward<Token>(token)); }

    template<typename Option>
    void set_option(Option const& option, boost::system::error_code& error) { socket().set_option(option, error); }

    IoContextTcpSocket::endpoint_type remote_endpoint() const { return socket().remote_endpoint(); }
};
}

using RequestParser = boost::beast::http::request_parser<RequestBody>;

// Feeds parser what the buffer has: true once a request is whole. error: it can't be parsed, or its
// body is over the parser's limit
bool ParseRequest(MessageBuffer& buffer, RequestParser& parser, boost::system::error_code& error);
// The bytes of response; to a HEAD its head only
MessageBuffer SerializeResponse(Request const& request, Response const& response);
MessageBuffer SerializeRequest(Request const& request);

// The connection as a route handler sees it, whatever its stream
class AbstractSocket
{
public:
    virtual ~AbstractSocket() = default;

    // Its network thread, once for each request: for an Async one, posted there by whoever answered
    virtual void SendResponse(RequestContext& context) = 0;
    // "[203.0.113.7:51234]", for a log
    virtual std::string GetClientInfo() const = 0;
    virtual boost::asio::ip::address const& GetRemoteIpAddress() const = 0;
    virtual bool IsOpen() const = 0;
    virtual void CloseSocket() = 0;

protected:
    // The server.http log's lines of an exchange: debug the request line and status, trace their contents
    void LogExchange(RequestContext const& context, MessageBuffer const& response) const;
};

template<typename Stream>
class BaseSocket : public Net::Socket<Stream>, public AbstractSocket, private WebSocket::ConnectionTransport
{
    using Base = Net::Socket<Stream>;

public:
    using Base::Base;

    // Before Start(). Beast's default is 1 MiB
    void SetMaxRequestBodySize(std::uint64_t size) { _maxBodySize = size; }

    // Before Start(), 0 turns one off. request: from the connection (its TLS handshake included), or a
    // request's first byte, to the end of its headers, then between two reads of its body. keepAlive:
    // from a response to the next request
    void SetTimeouts(Milliseconds request, Milliseconds keepAlive)
    {
        _requestTimeout = request;
        _keepAliveTimeout = keepAlive;
        this->SetDeadline(_requestTimeout);
    }

    // What Start() does, after the TLS handshake if there is one
    void StartHttp()
    {
        NewParser();
        this->StartReading();
    }

    void Start() override { StartHttp(); }

    bool Update() override
    {
        if (_streaming.Body)
            QueueStreamed();
        if (_webSocket)
            _webSocket->Flush(*this);

        if (Base::Update())
            return true;
        if (_webSocket)
            _webSocket->Closed();
        return false;
    }

    virtual RequestHandlerResult RequestHandler(RequestContext& context) = 0;

    void SendResponse(RequestContext& context) final
    {
        // A streamed body sends its length, its pieces come after the head
        bool streamed = context.responseStream && context.request.method() != boost::beast::http::verb::head;
        if (context.responseStream)
            context.response.content_length(context.responseStream->Length);
        else
            context.response.prepare_payload();

        MessageBuffer bytes = SerializeResponse(context.request, context.response);
        _lastStatus = context.response.result();
        LogExchange(context, bytes);
        this->QueuePacket(std::move(bytes));

        if (streamed && context.responseStream->Length > 0)
        {
            // Done once all of it is queued: the next request waits, a Connection: close waits
            _streaming = { .Body = std::move(context.responseStream), .KeepAlive = context.response.keep_alive() };
            QueueStreamed();
            return;
        }
        ResponseQueued(context.response.keep_alive());
    }

    // Called by a handler answering a WebSocket handshake with 101 (WebSocket::AcceptHandshake). Once
    // that response is queued what is read goes to connection and what it sends is written; idleTimeout
    // (0: none) and maxWriteQueueSize (0: no limit) replace the HTTP timeouts. Any other status drops it
    void UpgradeToWebSocket(std::shared_ptr<WebSocket::Connection> connection, Milliseconds idleTimeout, std::size_t maxWriteQueueSize)
    {
        _upgrade = { .Connection = std::move(connection), .IdleTimeout = idleTimeout, .MaxWriteQueueSize = maxWriteQueueSize };
    }

    bool IsWebSocket() const { return _webSocket != nullptr; }

    std::string GetClientInfo() const override { return StringFormat("[{}:{}]", this->GetRemoteIpAddress(), this->GetRemotePort()); }
    boost::asio::ip::address const& GetRemoteIpAddress() const final { return Base::GetRemoteIpAddress(); }
    bool IsOpen() const final { return Base::IsOpen(); }
    void CloseSocket() final { Base::CloseSocket(); }

protected:
    SocketReadCallbackResult ReadHandler() final
    {
        if (_webSocket)
            return _webSocket->Read(*this, this->GetReadBuffer());
        if (!HandleReadRequests())
            return SocketReadCallbackResult::Stop;

        // Answered later: no more reading until it is, ResponseQueued reads on
        if (_answering)
        {
            _readingPaused = true;
            return SocketReadCallbackResult::Stop;
        }
        return SocketReadCallbackResult::KeepReading;
    }

private:
    enum class Phase
    {
        Idle,           // between two requests
        Headers,        // a request's headers (or the connection is new)
        Body,
        Handling
    };

    void NewParser()
    {
        _parser.emplace();
        _parser->eager(true);
        if (_maxBodySize)
            _parser->body_limit(*_maxBodySize);
    }

    // The whole requests of the read buffer, until one is answered later. False when the connection
    // closes for a malformed request or a handler's error
    bool HandleReadRequests()
    {
        MessageBuffer& buffer = this->GetReadBuffer();
        while (!_answering && !_webSocket && this->IsOpen() && buffer.GetActiveSize() > 0)
        {
            if (_phase == Phase::Idle)
            {
                _phase = Phase::Headers;
                this->SetDeadline(_requestTimeout);
            }

            boost::system::error_code error;
            if (!ParseRequest(buffer, *_parser, error))
            {
                if (error)
                {
                    RefuseUnparsable(error);
                    return false;
                }
                // The headers are in: a body may take long (an upload) as long as it keeps coming
                if (_phase == Phase::Headers && _parser->is_header_done())
                {
                    _phase = Phase::Body;
                    this->ClearDeadline();
                    this->SetIdleTimeout(_requestTimeout);
                }
                break;
            }

            // The handler's time isn't the client's
            _phase = Phase::Handling;
            this->ClearDeadline();
            this->SetIdleTimeout(0ms);
            if (!Handle(_parser->release()))
            {
                this->CloseSocket();
                return false;
            }
            NewParser();
        }

        // Frames the client sent right behind its handshake
        if (_webSocket && this->IsOpen() && buffer.GetActiveSize() > 0)
            return _webSocket->Read(*this, buffer) == SocketReadCallbackResult::KeepReading;
        return true;
    }

    bool Handle(Request&& request)
    {
        RequestContext context{ .request = std::move(request) };
        _answering = true;
        _insideHandler = true;
        RequestHandlerResult result = RequestHandler(context);
        if (result != RequestHandlerResult::Async)
            SendResponse(context);
        _insideHandler = false;
        return result != RequestHandlerResult::Error;
    }

    // The response is in the write queue: on to the next request
    void ResponseQueued(bool keepAlive)
    {
        if (!keepAlive)
            this->DelayedCloseSocket();

        _answering = false;
        _phase = Phase::Idle;
        this->SetIdleTimeout(0ms);
        this->SetDeadline(_keepAliveTimeout);
        if (_upgrade.Connection)
            Upgrade();

        // Inside the handler HandleReadRequests goes on by itself
        if (_insideHandler || !this->IsOpen() || !HandleReadRequests())
            return;
        if (_readingPaused && !_answering)
        {
            _readingPaused = false;
            this->StartReading();
        }
    }

    // Queues pieces while little waits to be written: a client reading slowly holds StreamQueueSize
    // bytes, not the file. The reads block, so a local file
    void QueueStreamed()
    {
        ResponseStream& body = *_streaming.Body;
        while (this->IsOpen() && body.Length > 0 && this->GetWriteQueueSize() < StreamQueueSize)
        {
            std::size_t size = std::size_t(std::min<std::uint64_t>(body.Length, StreamPieceSize));
            MessageBuffer piece(size);
            body.Stream->read(reinterpret_cast<char*>(piece.GetWritePointer()), std::streamsize(size));
            if (std::size_t(body.Stream->gcount()) != size)
            {
                // The head is out with its length: the client sees the body end short
                RT_LOG_ERROR("server.http", "{} The body's stream ended {} bytes early, closing", GetClientInfo(), body.Length);
                _streaming = {};
                this->CloseSocket();
                return;
            }
            piece.WriteCompleted(size);
            body.Length -= size;
            this->QueuePacket(std::move(piece));
        }

        if (!this->IsOpen())
            _streaming = {};
        else if (body.Length == 0)
            ResponseQueued(std::exchange(_streaming, {}).KeepAlive);
    }

    void Upgrade()
    {
        WebSocketUpgrade upgrade = std::exchange(_upgrade, {});
        if (_lastStatus != boost::beast::http::status::switching_protocols || !this->IsOpen())
            return;

        _webSocket = std::move(upgrade.Connection);
        this->ClearDeadline();
        this->SetIdleTimeout(upgrade.IdleTimeout);
        this->SetMaxWriteQueueSize(upgrade.MaxWriteQueueSize);
        _webSocket->Open(*this);
    }

    // 400, or 413 for a body over the limit, then closes once it is written
    void RefuseUnparsable(boost::system::error_code const& error)
    {
        RT_LOG_DEBUG("server.http", "{} sent a request that can't be parsed: {}", GetClientInfo(), error.message());
        RequestContext context;
        context.request.version(11);
        context.response.version(11);
        context.response.result(error == boost::beast::http::error::body_limit ? boost::beast::http::status::payload_too_large
            : boost::beast::http::status::bad_request);
        context.response.keep_alive(false);

        _answering = true;
        _insideHandler = true;
        SendResponse(context);
        _insideHandler = false;
        _readingPaused = false;
    }

    // WebSocket::ConnectionTransport
    void WriteFrame(MessageBuffer&& frame) override { this->QueuePacket(std::move(frame)); }
    void CloseAfterWrites() override { this->DelayedCloseSocket(); }
    bool IsTransportOpen() const override { return this->IsOpen(); }
    std::string GetPeerInfo() const override { return GetClientInfo(); }

    struct WebSocketUpgrade
    {
        std::shared_ptr<WebSocket::Connection> Connection;
        Milliseconds IdleTimeout = 0ms;
        std::size_t MaxWriteQueueSize = 0;
    };

    struct StreamedBody
    {
        std::shared_ptr<ResponseStream> Body;
        bool KeepAlive = true;
    };

    static constexpr std::size_t StreamPieceSize = 256 * 1024;
    static constexpr std::size_t StreamQueueSize = 1024 * 1024;

    Optional<RequestParser> _parser;
    Optional<std::uint64_t> _maxBodySize;
    Milliseconds _requestTimeout = 0ms;
    Milliseconds _keepAliveTimeout = 0ms;
    Phase _phase = Phase::Headers;
    bool _answering = false;            // a request's response isn't queued yet
    bool _insideHandler = false;
    bool _readingPaused = false;

    StreamedBody _streaming;
    WebSocketUpgrade _upgrade;          // asked for by the handler, until its response is queued
    boost::beast::http::status _lastStatus = boost::beast::http::status::ok;
    std::shared_ptr<WebSocket::Connection> _webSocket;
};
}

#endif // ROOFTREE_BASE_HTTP_SOCKET_H
