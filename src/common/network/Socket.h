/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A TCP connection owned by one network thread (NetworkThread.h), over a Stream: the TCP socket itself,
 * TLS on it (SslStream.h) or either (OptionalSslStream.h).
 *
 *   reading  StartReading() runs a coroutine that reads into GetReadBuffer() and calls ReadHandler()
 *            after every read, until it returns Stop or the connection closes
 *   writing  QueuePacket() queues bytes; the thread's Update(), every millisecond, writes what it can
 *            without blocking and waits for the socket to take more when it is full
 *   closing  CloseSocket() at once, DelayedCloseSocket() once the queue is written. Update() then
 *            cancels what still waits and returns false, the thread lets go of the socket; the
 *            coroutines hold it until they end, right after
 *
 * Every member is for the socket's network thread, unless said otherwise. A connection may be given
 * a deadline, an idle timeout and a largest write queue; Update() closes it past them.
 */

#ifndef ROOFTREE_SOCKET_H
#define ROOFTREE_SOCKET_H

#include "Duration.h"
#include "IoContext.h"
#include "IpAddress.h"
#include "Log.h"
#include "MessageBuffer.h"
#include "Task.h"
#include <boost/asio/ip/tcp.hpp>
#include <atomic>
#include <memory>
#include <queue>

namespace Rooftree::Net
{
using IoContextTcpSocket = boost::asio::basic_stream_socket<boost::asio::ip::tcp, Asio::IoContextExecutor>;

enum class SocketReadCallbackResult
{
    KeepReading,
    Stop
};

template<typename Stream = IoContextTcpSocket>
class Socket : public std::enable_shared_from_this<Socket<Stream>>
{
public:
    // An accepted socket, and what Stream takes besides it (a TLS context)
    template<typename... Args>
    explicit Socket(IoContextTcpSocket&& socket, Args&&... args)
        : _stream(std::move(socket), std::forward<Args>(args)...), _remoteAddress(_stream.remote_endpoint().address()),
        _remotePort(_stream.remote_endpoint().port()) { }

    Socket(Socket const&) = delete;
    Socket& operator=(Socket const&) = delete;

    virtual ~Socket()
    {
        boost::system::error_code ignored;
        _stream.close(ignored);
    }

    // Once accepted, before the thread owns it: what the connection does first (a handshake, reading)
    virtual void Start() { }

    // The network thread's, every millisecond: writes what is queued. False once closed
    virtual bool Update()
    {
        if (_state == State::Closed)
        {
            // What still waits ends now (a wait to write to a peer that doesn't read would hold the
            // socket, and its file, as long as the peer stays): its coroutine lets go of the socket
            boost::system::error_code ignored;
            _stream.cancel(ignored);
            return false;
        }

        if (TimedOut())
        {
            RT_LOG_DEBUG("network", "Socket: {}:{} timed out, closing", _remoteAddress, _remotePort);
            CloseSocket();
            return false;
        }

        if (!_waitingToWrite)
            WriteQueued();
        return true;
    }

    boost::asio::ip::address const& GetRemoteIpAddress() const { return _remoteAddress; }
    uint16 GetRemotePort() const { return _remotePort; }

    void QueuePacket(MessageBuffer&& bytes)
    {
        _queuedBytes += bytes.GetActiveSize();
        _writes.push(std::move(bytes));
        if (_maxQueuedBytes && _queuedBytes > _maxQueuedBytes && IsOpen())
        {
            RT_LOG_DEBUG("network", "Socket: {}:{} doesn't read, {} bytes queued, closing", _remoteAddress, _remotePort, _queuedBytes);
            CloseSocket();
        }
    }

    // Any thread
    bool IsOpen() const { return _state == State::Open; }

    // Any thread. Sends the peer an end of stream; the socket itself closes with this object
    void CloseSocket()
    {
        if (_state.exchange(State::Closed) == State::Closed)
            return;

        boost::system::error_code error;
        _stream.shutdown(boost::asio::socket_base::shutdown_send, error);
        if (error)
            RT_LOG_DEBUG("network", "Socket: {}:{} shutdown: {}", _remoteAddress, _remotePort, error.message());
        OnClose();
    }

    // Once what is queued is written
    void DelayedCloseSocket()
    {
        State open = State::Open;
        if (_state.compare_exchange_strong(open, State::Closing) && _writes.empty())
            CloseSocket();
    }

    MessageBuffer& GetReadBuffer() { return _readBuffer; }

    // Closed by Update() at deadline, or when nothing was read for the idle timeout. Off until set
    void SetDeadline(TimePoint deadline) { _deadline = deadline; }
    // 0: none
    void SetDeadline(Milliseconds fromNow) { _deadline = fromNow > 0ms ? TimePoint::clock::now() + fromNow : TimePoint::max(); }
    void ClearDeadline() { _deadline = TimePoint::max(); }
    void SetIdleTimeout(Milliseconds timeout)
    {
        _idleTimeout = timeout;
        _lastRead = TimePoint::clock::now();
    }

    // More bytes queued than this close the connection (a peer that doesn't read); 0 is no limit
    void SetMaxWriteQueueSize(std::size_t bytes) { _maxQueuedBytes = bytes; }
    std::size_t GetWriteQueueSize() const { return _queuedBytes; }

    // Kept as long as the socket lives: ConnectionLimiter counts the connection until then
    void SetConnectionTicket(std::shared_ptr<void> ticket) { _ticket = std::move(ticket); }

    Stream& underlying_stream() { return _stream; }

protected:
    virtual void OnClose() { }
    virtual SocketReadCallbackResult ReadHandler() { return SocketReadCallbackResult::KeepReading; }

    void StartReading()
    {
        if (IsOpen())
            ReadLoop(this->shared_from_this());
    }

private:
    enum class State : uint8 { Open, Closing, Closed };

    // What ReadHandler throws closes this connection, nothing else
    static Rooftree::Detached ReadLoop(std::shared_ptr<Socket> self)
    {
        while (self->IsOpen())
        {
            auto [error, bytes] = co_await Rooftree::Await<boost::system::error_code, std::size_t>([&](auto done)
            {
                MessageBuffer& buffer = self->_readBuffer;
                buffer.Normalize();
                buffer.EnsureFreeSpace();
                self->_stream.async_read_some(boost::asio::buffer(buffer.GetWritePointer(), buffer.GetRemainingSpace()), std::move(done));
            });
            if (error)
            {
                self->CloseSocket();
                co_return;
            }

            self->_readBuffer.WriteCompleted(bytes);
            if (self->_idleTimeout > 0ms)
                self->_lastRead = TimePoint::clock::now();
            if (!self->IsOpen())
                co_return;

            try
            {
                if (self->ReadHandler() == SocketReadCallbackResult::Stop)
                    co_return;
            }
            catch (std::exception const& e)
            {
                RT_LOG_ERROR("network", "Socket: {}:{} threw: {}, closing", self->_remoteAddress, self->_remotePort, e.what());
                self->CloseSocket();
                co_return;
            }
        }
    }

    // The socket's buffer is full: on once it takes more. An error shows in the next write
    static Rooftree::Detached WaitToWrite(std::shared_ptr<Socket> self)
    {
        self->_waitingToWrite = true;
        co_await Rooftree::Await<boost::system::error_code>([&](auto done)
        {
            self->_stream.async_wait(boost::asio::socket_base::wait_type::wait_write, std::move(done));
        });
        self->_waitingToWrite = false;
        self->WriteQueued();
    }

    // As much as the socket takes now
    void WriteQueued()
    {
        while (!_writes.empty())
        {
            MessageBuffer& next = _writes.front();
            boost::system::error_code error;
            std::size_t written = _stream.write_some(boost::asio::buffer(next.GetReadPointer(), next.GetActiveSize()), error);
            if (error == boost::asio::error::would_block || error == boost::asio::error::try_again)
            {
                WaitToWrite(this->shared_from_this());
                return;
            }

            // A broken connection drops what it can't write; the read loop sees the error and closes it
            if (error || written == 0 || written == next.GetActiveSize())
            {
                _queuedBytes -= next.GetActiveSize();
                _writes.pop();
                continue;
            }

            // Part of it: the socket is full, no use trying again right away
            next.ReadCompleted(written);
            _queuedBytes -= written;
            WaitToWrite(this->shared_from_this());
            return;
        }

        if (_state == State::Closing)
            CloseSocket();
    }

    bool TimedOut() const
    {
        // Every socket every millisecond: no clock without a timeout
        if (_deadline == TimePoint::max() && _idleTimeout == 0ms)
            return false;
        TimePoint now = TimePoint::clock::now();
        return now >= _deadline || (_idleTimeout > 0ms && now - _lastRead >= _idleTimeout);
    }

    Stream _stream;
    boost::asio::ip::address _remoteAddress;
    uint16 _remotePort;
    std::atomic<State> _state = State::Open;

    MessageBuffer _readBuffer = MessageBuffer(4096);
    std::queue<MessageBuffer> _writes;
    std::size_t _queuedBytes = 0;
    std::size_t _maxQueuedBytes = 0;
    bool _waitingToWrite = false;

    TimePoint _deadline = TimePoint::max();
    Milliseconds _idleTimeout = 0ms;
    TimePoint _lastRead;
    std::shared_ptr<void> _ticket;
};
}

#endif // ROOFTREE_SOCKET_H
