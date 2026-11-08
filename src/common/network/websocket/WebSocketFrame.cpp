/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "WebSocketFrame.h"
#include "CryptoHash.h"
#include <boost/beast/version.hpp>
#include <boost/beast/websocket/rfc6455.hpp>
#include <utf8.h>
#include <array>

namespace Rooftree::Net::WebSocket
{
namespace
{
constexpr std::string_view HandshakeGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

constexpr uint8 FinBit = 0x80;
constexpr uint8 RsvBits = 0x70;
constexpr uint8 OpcodeBits = 0x0F;
constexpr uint8 MaskBit = 0x80;
constexpr uint8 LengthBits = 0x7F;

constexpr std::size_t MaxControlPayloadSize = 125;

bool IsKnownOpcode(uint8 opcode)
{
    switch (Opcode(opcode))
    {
        case Opcode::Continuation:
        case Opcode::Text:
        case Opcode::Binary:
        case Opcode::Close:
        case Opcode::Ping:
        case Opcode::Pong:
            return true;
        default:
            return false;
    }
}

// Codes a peer may send, RFC 6455 section 7.4
bool IsValidReceivedCloseCode(uint16 code)
{
    if (code >= 3000 && code <= 4999)   // registered and private use
        return true;

    switch (code)
    {
        case 1000: case 1001: case 1002: case 1003: case 1007:
        case 1008: case 1009: case 1010: case 1011:
            return true;
        default:
            return false;
    }
}

std::string Base64Encode(uint8 const* data, std::size_t size)
{
    static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string result;
    result.reserve((size + 2) / 3 * 4);
    for (std::size_t i = 0; i < size; i += 3)
    {
        uint32 chunk = uint32(data[i]) << 16;
        if (i + 1 < size)
            chunk |= uint32(data[i + 1]) << 8;
        if (i + 2 < size)
            chunk |= data[i + 2];

        result += Alphabet[(chunk >> 18) & 0x3F];
        result += Alphabet[(chunk >> 12) & 0x3F];
        result += i + 1 < size ? Alphabet[(chunk >> 6) & 0x3F] : '=';
        result += i + 2 < size ? Alphabet[chunk & 0x3F] : '=';
    }
    return result;
}
}

ParseResult ParseClientFrame(MessageBuffer& buffer, Frame& frame, std::size_t maxPayloadSize, CloseCode& error)
{
    std::size_t available = buffer.GetActiveSize();
    uint8 const* data = buffer.GetReadPointer();
    if (available < 2)
        return ParseResult::NeedMoreData;

    uint8 opcode = data[0] & OpcodeBits;
    bool fin = (data[0] & FinBit) != 0;
    bool masked = (data[1] & MaskBit) != 0;

    // No extension is ever negotiated, so a set RSV bit can't mean anything
    if ((data[0] & RsvBits) || !IsKnownOpcode(opcode) || !masked)
    {
        error = CloseCode::ProtocolError;
        return ParseResult::Error;
    }

    std::size_t headerSize = 2;
    uint64 payloadSize = data[1] & LengthBits;
    if (payloadSize == 126)
    {
        headerSize += 2;
        if (available < headerSize)
            return ParseResult::NeedMoreData;

        payloadSize = (uint64(data[2]) << 8) | data[3];
    }
    else if (payloadSize == 127)
    {
        headerSize += 8;
        if (available < headerSize)
            return ParseResult::NeedMoreData;

        payloadSize = 0;
        for (std::size_t i = 0; i < 8; ++i)
            payloadSize = (payloadSize << 8) | data[2 + i];

        // The most significant bit must be 0
        if (payloadSize & (uint64(1) << 63))
        {
            error = CloseCode::ProtocolError;
            return ParseResult::Error;
        }
    }

    if (IsControl(Opcode(opcode)) && (!fin || payloadSize > MaxControlPayloadSize))
    {
        error = CloseCode::ProtocolError;
        return ParseResult::Error;
    }

    if (payloadSize > maxPayloadSize)
    {
        error = CloseCode::MessageTooBig;
        return ParseResult::Error;
    }

    std::array<uint8, 4> mask;
    std::copy_n(data + headerSize, mask.size(), mask.begin());
    headerSize += mask.size();

    if (available < headerSize + payloadSize)
        return ParseResult::NeedMoreData;

    frame.Fin = fin;
    frame.Type = Opcode(opcode);
    frame.Payload.resize(payloadSize);
    for (std::size_t i = 0; i < payloadSize; ++i)
        frame.Payload[i] = char(data[headerSize + i] ^ mask[i % 4]);

    buffer.ReadCompleted(headerSize + payloadSize);
    return ParseResult::Done;
}

MessageBuffer SerializeFrame(Opcode opcode, std::string_view payload, bool fin /*= true*/)
{
    std::array<uint8, 10> header;
    std::size_t headerSize = 2;
    header[0] = uint8(opcode) | (fin ? FinBit : 0);
    if (payload.size() < 126)
        header[1] = uint8(payload.size());
    else if (payload.size() <= 0xFFFF)
    {
        header[1] = 126;
        header[2] = uint8(payload.size() >> 8);
        header[3] = uint8(payload.size());
        headerSize += 2;
    }
    else
    {
        header[1] = 127;
        for (std::size_t i = 0; i < 8; ++i)
            header[2 + i] = uint8(uint64(payload.size()) >> (56 - 8 * i));
        headerSize += 8;
    }

    MessageBuffer buffer(headerSize + payload.size());
    buffer.Write(header.data(), headerSize);
    buffer.Write(payload.data(), payload.size());
    return buffer;
}

MessageBuffer SerializeCloseFrame(CloseCode code, std::string_view reason)
{
    if (code == CloseCode::None)
        return SerializeFrame(Opcode::Close, {});

    // The whole control frame payload is limited to 125 bytes, 2 of them are the code
    reason = reason.substr(0, MaxControlPayloadSize - 2);

    std::string payload;
    payload.reserve(2 + reason.size());
    payload += char(uint16(code) >> 8);
    payload += char(uint16(code) & 0xFF);
    payload += reason;
    return SerializeFrame(Opcode::Close, payload);
}

bool ParseClosePayload(std::string_view payload, CloseCode& code, std::string_view& reason)
{
    reason = {};
    if (payload.empty())
    {
        code = CloseCode::None;
        return true;
    }

    if (payload.size() < 2)
        return false;

    uint16 value = uint16(uint8(payload[0]) << 8) | uint8(payload[1]);
    if (!IsValidReceivedCloseCode(value))
        return false;

    reason = payload.substr(2);
    if (!IsValidUtf8(reason))
        return false;

    code = CloseCode(value);
    return true;
}

std::string ComputeAcceptKey(std::string_view key)
{
    Crypto::SHA1::Digest digest = Crypto::SHA1::GetDigestOf(key, HandshakeGuid);
    return Base64Encode(digest.data(), digest.size());
}

bool IsValidUtf8(std::string_view text)
{
    return utf8::is_valid(text.begin(), text.end());
}

bool AcceptHandshake(Http::Request const& request, Http::Response& response)
{
    namespace http = boost::beast::http;

    response.version(request.version());
    response.set(http::field::server, BOOST_BEAST_VERSION_STRING);

    // GET, HTTP/1.1, "Connection: upgrade" and "Upgrade: websocket"
    if (!boost::beast::websocket::is_upgrade(request))
    {
        response.result(http::status::bad_request);
        return false;
    }

    if (request[http::field::sec_websocket_version] != "13")
    {
        response.result(http::status::upgrade_required);
        response.set(http::field::sec_websocket_version, "13");
        return false;
    }

    // 16 random bytes, base64 encoded
    std::string_view key = Http::ToStdStringView(request[http::field::sec_websocket_key]);
    if (key.size() != 24)
    {
        response.result(http::status::bad_request);
        return false;
    }

    response.result(http::status::switching_protocols);
    response.set(http::field::upgrade, "websocket");
    response.set(http::field::connection, "Upgrade");
    response.set(http::field::sec_websocket_accept, ComputeAcceptKey(key));
    return true;
}
}
