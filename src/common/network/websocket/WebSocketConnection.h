/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A WebSocket connection once its opening handshake is done, apart from the socket it
 * runs on. Two kinds of socket run one:
 *
 *  - WebSocket::BaseSocket, a socket of its own port that reads the handshake itself (WebSocketSocket.h)
 *  - an HTTP socket that answered a handshake request with 101 (Http::BaseSocket::UpgradeToWebSocket),
 *    so pages and their WebSocket share one port, one TLS setup and one origin
 *
 * The connection parses the frames the socket read, joins fragments, answers pings and the closing
 * handshake, and hands complete messages to OnMessage. What it sends goes through a lock-free queue
 * (SendText from any thread) that the socket moves into its write
 * queue on its network thread.
 */

#ifndef ROOFTREE_WEBSOCKET_CONNECTION_H
#define ROOFTREE_WEBSOCKET_CONNECTION_H

#include "MPSCQueue.h"
#include "Socket.h"
#include "WebSocketFrame.h"
#include <atomic>

namespace Rooftree::Net::WebSocket
{
// The socket running a connection, as the connection sees it. Network thread only
class ConnectionTransport
{
public:
    virtual void WriteFrame(MessageBuffer&& frame) = 0;     // into the socket's write queue
    virtual void CloseAfterWrites() = 0;                    // DelayedCloseSocket
    virtual bool IsTransportOpen() const = 0;
    virtual std::string GetPeerInfo() const = 0;            // for logs

protected:
    ~ConnectionTransport() = default;
};

class Connection
{
public:
    Connection() = default;
    Connection(Connection const&) = delete;
    Connection& operator=(Connection const&) = delete;
    virtual ~Connection() = default;

    // Thread safe
    void SendText(std::string_view text) { Enqueue(SerializeFrame(Opcode::Text, text), false); }
    void SendBinary(std::string_view data) { Enqueue(SerializeFrame(Opcode::Binary, data), false); }

    // Thread safe. Sends a close frame, the connection is closed once it is written
    void Close(CloseCode code = CloseCode::Normal, std::string_view reason = {}) { Enqueue(SerializeCloseFrame(code, reason), true); }

    // Largest message accepted, after joining fragments; bigger ones close the connection with 1009
    void SetMaxMessageSize(std::size_t size) { _maxMessageSize = size; }

    // Any thread: from Open() until the socket closed
    bool IsConnected() const { return _connected; }

    // For the socket running the connection, on its network thread:
    // the 101 response is queued, messages may be sent from now on (OnOpen)
    void Open(ConnectionTransport& transport);
    // The frames read into buffer; Stop when the connection is being closed
    SocketReadCallbackResult Read(ConnectionTransport& transport, MessageBuffer& buffer);
    // What was sent since, into the socket's write queue (the socket's Update())
    void Flush(ConnectionTransport& transport);
    // The socket closed
    void Closed() { _connected = false; }

protected:
    // The 101 response is queued, messages may be sent from now on
    virtual void OnOpen() { }

    // A complete Text (valid UTF-8) or Binary message
    virtual void OnMessage(Opcode opcode, std::string&& payload) = 0;

private:
    static constexpr std::size_t DefaultMaxMessageSize = 64 * 1024;

    struct QueuedFrame
    {
        MessageBuffer Buffer;
        bool CloseAfter;
    };

    void Enqueue(MessageBuffer&& buffer, bool closeAfter)
    {
        _sendQueue.Enqueue(new QueuedFrame{ .Buffer = std::move(buffer), .CloseAfter = closeAfter });
    }

    // Unlike Close() it bypasses the send queue
    void Fail(ConnectionTransport& transport, CloseCode code, std::string_view reason);

    // false if the connection is being closed
    bool HandleFrame(ConnectionTransport& transport, Frame&& frame);
    bool Deliver(ConnectionTransport& transport, Opcode opcode, std::string&& payload);

    std::atomic<bool> _connected = false;
    bool _closeSent = false;
    std::size_t _maxMessageSize = DefaultMaxMessageSize;

    Opcode _fragmentType = Opcode::Continuation;    // Continuation = no fragmented message in progress
    std::string _fragments;

    MPSCQueue<QueuedFrame> _sendQueue;
};
}

#endif // ROOFTREE_WEBSOCKET_CONNECTION_H
