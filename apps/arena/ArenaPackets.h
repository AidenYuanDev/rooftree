/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The arena's messages are FlatBuffers (schema/Arena.fbs) in the server core's packets (Packet.h):
 * the session's handlers read the tables, World and the session build them. What a transport carries:
 *
 *   KCP, a WebSocket's binary messages      the packets as they are
 *   a WebSocket's text messages (the page)   {"type": name, ...fields} JSON, converted here by the
 *                                            schema, nothing written per message
 *
 * A WebSocket answers in JSON unless the handshake asked for format=packets (a native client whose
 * UDP is blocked). A client's message is read and verified on the network thread, so the world
 * thread only gets valid tables.
 */

#ifndef ARENA_PACKETS_H
#define ARENA_PACKETS_H

#include "Arena_generated.h"
#include "Define.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// To clients: the packet, and its JSON for the pages, made the first time one is sent to
class ServerPacket
{
public:
    // A finished builder of the opcode's table
    ServerPacket(Arena::Opcode opcode, flatbuffers::FlatBufferBuilder const& builder);

    Arena::Opcode GetOpcode() const { return _opcode; }
    std::string const& GetPacket() const { return _packet; }

    // World thread (where everything is sent): converted once for all the pages a broadcast goes to
    std::string const& GetJson() const;

private:
    Arena::Opcode _opcode;
    std::string _packet;
    mutable std::string _json;
};

ServerPacket MakeSystemMessage(std::string_view text);

// From a client: a verified table of the opcode's type
struct ClientMessage
{
    Arena::Opcode Opcode;
    std::vector<uint8> Payload;     // its own allocation: 8-aligned, as the tables are read in place

    template <class Table>
    Table const& Get() const { return *flatbuffers::GetRoot<Table>(Payload.data()); }
};

enum class ClientMessageResult
{
    Ok,
    Unknown,    // an opcode or type we don't know, a newer client: ignored
    Invalid     // not a packet, a table that fails the verifier, not JSON: the connection is closed
};

// Network thread. A packet: header + table
ClientMessageResult ReadClientPacket(std::string_view packet, std::unique_ptr<ClientMessage>& message);
// Network thread. A JSON object with a string "type"; fields of the wrong type are left out (0, "")
ClientMessageResult ReadClientJson(std::string_view text, std::unique_ptr<ClientMessage>& message);

// For logs: "Ping", "0x0123" if none
std::string GetOpcodeName(Arena::Opcode opcode);

#endif // ARENA_PACKETS_H
