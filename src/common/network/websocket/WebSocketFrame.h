/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The server side of RFC 6455, the WebSocket protocol, written against MessageBuffer
 * like the rest of the net module.
 *
 * Boost.Beast's websocket::stream is not used: it runs its own async read/write loop on the stream,
 * while Rooftree::Net::Socket owns that loop (AsyncRead -> ReadHandler, QueuePacket -> Update). So the
 * opening handshake is parsed with Beast's HTTP parser and the frames with the functions here, then the
 * socket stays a plain Rooftree::Net::Socket that NetworkThread updates like every other one.
 *
 * Not supported: extensions (permessage-deflate is never negotiated, so RSV bits must be 0) and
 * subprotocols (OnHandshake may still pick one and set Sec-WebSocket-Protocol itself).
 */

#ifndef ROOFTREE_WEBSOCKET_FRAME_H
#define ROOFTREE_WEBSOCKET_FRAME_H

#include "Define.h"
#include "HttpCommon.h"
#include "MessageBuffer.h"
#include <string>
#include <string_view>

namespace Rooftree::Net::WebSocket
{
enum class Opcode : uint8
{
    Continuation    = 0x0,
    Text            = 0x1,
    Binary          = 0x2,
    Close           = 0x8,
    Ping            = 0x9,
    Pong            = 0xA
};

constexpr bool IsControl(Opcode opcode) { return (uint8(opcode) & 0x8) != 0; }

// RFC 6455 section 7.4.1
enum class CloseCode : uint16
{
    None            = 0,    // never sent, a close frame without a body
    Normal          = 1000,
    GoingAway       = 1001,
    ProtocolError   = 1002,
    UnsupportedData = 1003,
    InvalidPayload  = 1007, // e.g. a text message that is not UTF-8
    PolicyViolation = 1008,
    MessageTooBig   = 1009,
    InternalError   = 1011
};

struct Frame
{
    bool Fin = false;
    Opcode Type = Opcode::Continuation;
    std::string Payload;    // unmasked
};

enum class ParseResult
{
    Done,           // frame is filled and its bytes are consumed from the buffer
    NeedMoreData,   // nothing consumed
    Error           // error says which close code to send
};

// Reads one client frame. Clients must mask every frame, unmasked ones are a protocol error
ParseResult ParseClientFrame(MessageBuffer& buffer, Frame& frame, std::size_t maxPayloadSize, CloseCode& error);

// A server frame, never masked
MessageBuffer SerializeFrame(Opcode opcode, std::string_view payload, bool fin = true);

MessageBuffer SerializeCloseFrame(CloseCode code, std::string_view reason);

// Close code and reason of a received close frame payload, false if it is malformed
bool ParseClosePayload(std::string_view payload, CloseCode& code, std::string_view& reason);

// Sec-WebSocket-Accept for a Sec-WebSocket-Key: base64(SHA1(key + RFC 6455 GUID))
std::string ComputeAcceptKey(std::string_view key);

bool IsValidUtf8(std::string_view text);

// Checks the opening handshake request and fills response with 101 Switching Protocols, or with the
// error status to answer with (400, or 426 with Sec-WebSocket-Version for an unsupported version)
bool AcceptHandshake(Http::Request const& request, Http::Response& response);
}

#endif // ROOFTREE_WEBSOCKET_FRAME_H
