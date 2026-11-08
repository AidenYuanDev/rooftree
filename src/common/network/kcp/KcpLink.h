/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What both ends of a KCP connection do with its datagrams (KcpProtocol.h). A Link
 * seals what goes out and opens what comes in, and runs the KCP control block for the reliable
 * messages. It sends through a function, the UDP socket belongs to its Server or Client; it is used
 * by one thread only, theirs.
 */

#ifndef ROOFTREE_KCP_LINK_H
#define ROOFTREE_KCP_LINK_H

#include "Duration.h"
#include "KcpProtocol.h"
#include <functional>

struct IKCPCB;

namespace Rooftree::Net::Kcp
{
// KCP tuned for games by default: ikcp_nodelay(1, 10, 2, 1) as its README suggests
struct Settings
{
    int32 Interval = 10;                    // ms between two KCP updates (retransmission timers)
    bool NoDelay = true;                    // RTO doesn't double on loss, minimum RTO 30 ms instead of 100
    int32 FastResend = 2;                   // resend a segment skipped by this many acknowledgments, 0 = off
    bool CongestionControl = false;         // KCP's congestion window: off, a game sends little
    int32 SendWindow = 128;                 // segments
    int32 ReceiveWindow = 128;              // segments
    std::size_t MaxMessageSize = 64 * 1024; // reliable messages. Larger ones close the connection
    uint32 MaxWaitingSegments = 2048;       // unacknowledged, more closes the connection (SendBufferFull)
    Milliseconds IdleTimeout = 15s;         // no datagram from the peer for this long closes the connection
    Milliseconds PingInterval = 5s;         // client: a Ping after this long without sending anything
    Milliseconds HelloInterval = 250ms;     // client: Hello repeated until the Welcome arrives
    Milliseconds HandshakeTimeout = 5s;     // client: no Welcome by then fails the connection
    Milliseconds CloseTimeout = 2s;         // Close(): how long the reliable data still queued may take

    // The largest MaxMessageSize KCP can carry with these windows
    std::size_t GetMaxMessageSizeLimit() const;
};

class Link
{
public:
    using Output = std::function<void(std::span<uint8 const> datagram)>;

    // sending: the direction of what this end sends. sendCounter: the first counter it sends, past the
    // client's Hellos, sealed before the conv was known
    Link(uint32 conv, Key const& key, Direction sending, Settings const& settings, Output output, uint64 sendCounter = 0);
    Link(Link const&) = delete;
    Link& operator=(Link const&) = delete;
    ~Link();

    uint32 GetConv() const { return _conv; }

    // A datagram for this link, with its header read: the body if it authenticates and wasn't seen
    // before, the counter is then remembered
    bool Open(std::span<uint8 const> datagram, Header const& header, std::size_t clearSize, std::vector<uint8>& body);

    // A counter seen before the link existed (the server's first Hello)
    void MarkSeen(uint64 counter) { _replay.Accept(counter); }
    // The highest counter of the peer accepted, a datagram above it is the newest
    uint64 GetHighestSeen() const { return _replay.GetHighest(); }

    // Seals and sends a datagram
    void Send(DatagramType type, std::span<uint8 const> body, std::span<uint8 const> clear = {});

    // A reliable message for KCP to send, false if it is larger than MaxMessageSize
    bool SendReliable(std::span<uint8 const> message);

    // A Data datagram's body. False if it is no valid KCP input
    bool Input(std::span<uint8 const> segments);

    enum class ReceiveResult
    {
        None,
        Message,
        TooBig
    };

    // The next complete reliable message
    ReceiveResult Receive(std::string& message);

    // Sends what KCP has queued and the acknowledgments due, now instead of at the next Update
    void Flush();

    // KCP's timers, at least every Interval ms (getMSTime)
    void Update(uint32 now);
    // When Update has to run next, at the latest
    uint32 Check(uint32 now) const;

    // Sent but not acknowledged yet
    uint32 GetWaitingSegments() const;
    // A segment was retransmitted too often
    bool IsDead() const;

private:
    static int KcpOutput(char const* buffer, int length, IKCPCB* kcp, void* user);

    uint32 _conv;
    Direction _sending;
    std::size_t _maxMessageSize;
    Output _output;
    Cipher _cipher;
    uint64 _sendCounter = 0;
    ReplayWindow _replay;
    IKCPCB* _kcp;
    std::vector<uint8> _datagram;       // scratch for Send
    bool _updated = false;              // ikcp_flush does nothing before the first ikcp_update
};
}

#endif // ROOFTREE_KCP_LINK_H
