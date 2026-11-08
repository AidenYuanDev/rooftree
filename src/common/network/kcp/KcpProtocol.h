/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The datagrams of a KCP connection (Kcp::Server, Kcp::Client). A game whose client we write ourselves
 * can take UDP, where a lost packet
 * delays only itself instead of everything after it (TCP's head of line blocking).
 *
 * Every datagram, little endian:
 *
 *   conv u32 | type u8 | counter u64 | clear part | body, encrypted | tag (16)
 *   \______________ header (13) ___/
 *
 *  - conv: the connection, chosen by the server. 0 only in a Hello, the client has none yet
 *  - counter: per direction, +1 for every datagram. It makes the AES-GCM nonce unique and the
 *    receiver drops what it saw already (a replayed datagram). A client starts at a random one, 63
 *    bits: a second client given the same key doesn't use the same nonces
 *  - clear part: only in a Hello, the token (u8 length + bytes) the server finds the key by
 *  - tag: AES-128-GCM over everything before it, the header and clear part as additional data
 *
 * The key is per login: the client got it with its token over the login's HTTPS, like a session key.
 * A datagram that doesn't authenticate is dropped without an answer, so nobody without the key gets
 * anything out of the server, and its answers are never larger than what it was sent (no use for a
 * reflection attack).
 *
 *   Hello       client -> server   body: version u8 + nonce u64. Repeated until the Welcome arrives,
 *                                  the nonce (random per connect) tells the repeats from another
 *                                  client with the same token
 *   Welcome     server -> client   body: version u8. The header carries the connection's conv
 *   Data        both               body: KCP segments, the reliable ordered messages
 *   Unreliable  both               body: one message, maybe lost, maybe out of order
 *   Ping, Pong  client <-> server  body: up to 8 bytes the Pong echoes (the client's clock)
 *   Close       both               body: code u16 + reason, the connection is gone
 */

#ifndef ROOFTREE_KCP_PROTOCOL_H
#define ROOFTREE_KCP_PROTOCOL_H

#include "Define.h"
#include <array>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

typedef struct evp_cipher_ctx_st EVP_CIPHER_CTX;

namespace Rooftree::Net::Kcp
{
enum class DatagramType : uint8
{
    Hello       = 1,
    Welcome     = 2,
    Data        = 3,
    Unreliable  = 4,
    Ping        = 5,
    Pong        = 6,
    Close       = 7
};

// Close codes, numbered like WebSocket's (an app may pass its own, 4000-4999)
namespace CloseCode
{
constexpr uint16 Normal             = 1000;
constexpr uint16 GoingAway          = 1001;
constexpr uint16 ProtocolError      = 1002;
constexpr uint16 PolicyViolation    = 1008;
constexpr uint16 MessageTooBig      = 1009;
constexpr uint16 InternalError      = 1011;
constexpr uint16 Timeout            = 3000;     // nothing heard for the idle timeout, or no Welcome
constexpr uint16 DeadLink           = 3001;     // a segment was sent too often without an acknowledgment
constexpr uint16 SendBufferFull     = 3002;     // the peer acknowledges slower than it is sent data
}

constexpr uint8 ProtocolVersion = 1;
constexpr std::size_t HelloBodySize = 1 + 8;
// The counter of the Close refusing another version's Hello: its token isn't used up, so a connection
// under the same key may follow, and that one counts from 0, never up to this
constexpr uint64 RefusalCounter = std::numeric_limits<uint64>::max();

constexpr std::size_t HeaderSize = 13;
constexpr std::size_t TagSize = 16;
constexpr std::size_t KeySize = 16;
constexpr std::size_t MaxTokenSize = 255;
constexpr std::size_t MaxCloseReasonSize = 123;     // as in a WebSocket close frame

// Largest datagram sent: fits the 1280 bytes every IPv6 path carries, with room for IP and UDP
// headers and a tunnel or two. Larger ones received are dropped
constexpr std::size_t MaxDatagramSize = 1200;
// The body of a Data or Unreliable datagram: KCP's MTU, the largest unreliable message
constexpr std::size_t MaxBodySize = MaxDatagramSize - HeaderSize - TagSize;

using Key = std::array<uint8, KeySize>;

enum class Direction : uint8
{
    ClientToServer = 0,
    ServerToClient = 1
};

struct Header
{
    uint32 Conv = 0;
    DatagramType Type = DatagramType::Hello;
    uint64 Counter = 0;
};

// Of a datagram at least HeaderSize long
Header ReadHeader(std::span<uint8 const> datagram);

// A Hello's token, empty if the datagram is too short for it. clearSize is what the clear part takes
std::string_view ReadHelloToken(std::span<uint8 const> datagram, std::size_t& clearSize);

// Close body
std::vector<uint8> MakeCloseBody(uint16 code, std::string_view reason);
bool ReadCloseBody(std::span<uint8 const> body, uint16& code, std::string& reason);

// AES-128-GCM under one connection's key. The nonce is the datagram's direction and counter, so the
// two directions never share one
class Cipher
{
public:
    explicit Cipher(Key const& key);
    Cipher(Cipher const&) = delete;
    Cipher& operator=(Cipher const&) = delete;
    ~Cipher();

    // header | clear | body encrypted | tag, into datagram (replacing what it held)
    void Seal(Direction direction, Header const& header, std::span<uint8 const> clear, std::span<uint8 const> body, std::vector<uint8>& datagram);

    // The body of a datagram whose first clearSize bytes after the header are clear, false if it
    // doesn't authenticate
    bool Open(Direction direction, std::span<uint8 const> datagram, std::size_t clearSize, std::vector<uint8>& body);

private:
    EVP_CIPHER_CTX* _seal;
    EVP_CIPHER_CTX* _open;
};

// The counters seen of one direction, like DTLS's: the highest one and a bitmap of the 64 before it.
// Older ones are dropped, they would be useless by now anyway
class ReplayWindow
{
public:
    bool IsNew(uint64 counter) const
    {
        if (!_any || counter > _highest)
            return true;
        uint64 age = _highest - counter;
        return age < 64 && !(_seen & (uint64(1) << age));
    }

    // After the datagram authenticated
    void Accept(uint64 counter)
    {
        if (!_any || counter > _highest)
        {
            uint64 shift = _any ? counter - _highest : 64;
            _seen = shift >= 64 ? 0 : _seen << shift;
            _seen |= 1;
            _highest = counter;
            _any = true;
            return;
        }

        _seen |= uint64(1) << (_highest - counter);
    }

    // The highest counter accepted, whether it is the newest datagram (address changes follow it)
    uint64 GetHighest() const { return _highest; }

private:
    uint64 _highest = 0;
    uint64 _seen = 0;
    bool _any = false;
};
}

#endif // ROOFTREE_KCP_PROTOCOL_H
