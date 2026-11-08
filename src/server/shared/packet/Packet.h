/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A packet is a binary WebSocket message (WorldWebSocket.h) for clients of our own: an opcode says
 * what it is, its payload is a FlatBuffer whose root type the
 * opcode's route knows. Little-endian, 8 bytes before the payload, which keeps it aligned as the
 * verifier wants (a buffer starts 8-aligned, so does its payload):
 *
 *   [uint16 opcode][uint16 flags, 0 for now][uint32 requestId][FlatBuffer]
 *
 * A request's answer has the same opcode and requestId, so a client can have many in flight; a
 * failed one is answered with opcode 0, a Rooftree.Packets.Error (Packets.fbs) naming the opcode. What
 * the server sends of itself (a notice) has requestId 0.
 *
 * A route checks its payload with a FlatBuffers verifier before its handler runs: FlatBuffers reads
 * in place without bounds checks, an unverified buffer could make it read anywhere.
 */

#ifndef ROOFTREE_PACKET_H
#define ROOFTREE_PACKET_H

#include "Define.h"
#include "Optional.h"
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rooftree::Server
{
inline constexpr uint16 ErrorOpcode = 0;
inline constexpr std::size_t PacketHeaderSize = 8;

struct PacketHeader
{
    uint16 Opcode = 0;
    uint16 Flags = 0;
    uint32 RequestId = 0;
};

// The header of data, none if it is shorter than one
Optional<PacketHeader> ReadPacketHeader(std::string_view data);

// The header and the payload, as they are sent
std::string WritePacket(PacketHeader header, std::span<uint8 const> payload);

// What an Error table says (Packets.fbs)
struct PacketError
{
    struct Detail
    {
        std::string Name;
        std::string Value;
        Optional<std::vector<std::string>> Values;  // a list instead of a value
    };

    uint16 Status = 0;                                      // like HTTP's
    std::string Code;
    std::string Message;
    std::vector<std::pair<std::string, std::string>> Fields;  // (name, code) of a validation_failed or conflict
    uint32 RetryAfter = 0;                                  // seconds
    std::vector<Detail> Details;
};

std::string MakeErrorPayload(uint16 opcode, PacketError const& error);
std::string MakeErrorPayload(uint16 opcode, uint16 status, std::string_view code, std::string_view message = {},
    std::vector<std::pair<std::string, std::string>> const& fields = {});

// An Error packet, with that table
std::string WriteErrorPacket(uint32 requestId, uint16 opcode, uint16 status, std::string_view code, std::string_view message = {},
    std::vector<std::pair<std::string, std::string>> const& fields = {});
}

#endif // ROOFTREE_PACKET_H
