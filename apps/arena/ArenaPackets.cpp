/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ArenaPackets.h"
#include "Arena_bfbs_generated.h"
#include "Errors.h"
#include "FlatJson.h"
#include "JsonHttp.h"
#include "Packet.h"
#include "StringFormat.h"
#include <boost/json/serialize.hpp>
#include <algorithm>
#include <span>

using Rooftree::Server::FlatJsonSchema;

namespace
{
// An opcode's JSON type and table. The page's types are its names of old, a client's "chat" is a
// Chat, the server's a ChatMessage
struct PacketType
{
    Arena::Opcode Opcode;
    std::string_view Type;
    std::string_view Table;
    bool (*Verify)(std::span<uint8 const> payload);     // client opcodes only
};

template <class Table>
bool VerifyTable(std::span<uint8 const> payload)
{
    flatbuffers::Verifier verifier(payload.data(), payload.size());
    return verifier.VerifyBuffer<Table>(nullptr);
}

constexpr PacketType ClientPackets[] =
{
    { Arena::Opcode::Ping,  "ping",  "Arena.Ping",  &VerifyTable<Arena::Ping> },
    { Arena::Opcode::Input, "input", "Arena.Input", &VerifyTable<Arena::Input> },
    { Arena::Opcode::Chat,  "chat",  "Arena.Chat",  &VerifyTable<Arena::Chat> },
};

constexpr PacketType ServerPackets[] =
{
    { Arena::Opcode::Welcome,       "welcome", "Arena.Welcome",       nullptr },
    { Arena::Opcode::Pong,          "pong",    "Arena.Pong",          nullptr },
    { Arena::Opcode::State,         "state",   "Arena.State",         nullptr },
    { Arena::Opcode::ChatMessage,   "chat",    "Arena.ChatMessage",   nullptr },
    { Arena::Opcode::SystemMessage, "system",  "Arena.SystemMessage", nullptr },
};

template <std::size_t N>
PacketType const* FindPacketType(PacketType const (&types)[N], Arena::Opcode opcode)
{
    auto itr = std::ranges::find(types, opcode, &PacketType::Opcode);
    return itr != std::end(types) ? &*itr : nullptr;
}

FlatJsonSchema const& GetArenaSchema()
{
    static FlatJsonSchema const schema = []
    {
        FlatJsonSchema loaded(Arena::StateBinarySchema::data());
        std::string error;
        bool ok = loaded.Load(error);
        ASSERT(ok, "Arena.fbs: %s", error.c_str());
        return loaded;
    }();
    return schema;
}
}

ServerPacket::ServerPacket(Arena::Opcode opcode, flatbuffers::FlatBufferBuilder const& builder) : _opcode(opcode),
    _packet(Rooftree::Server::WritePacket({ .Opcode = uint16(opcode) }, builder.GetBufferSpan()))
{
}

std::string const& ServerPacket::GetJson() const
{
    if (!_json.empty())
        return _json;

    PacketType const* type = FindPacketType(ServerPackets, _opcode);
    ASSERT(type, "no JSON type for server opcode %u", uint32(_opcode));

    FlatJsonSchema const& schema = GetArenaSchema();
    std::span<uint8 const> payload(reinterpret_cast<uint8 const*>(_packet.data()) + Rooftree::Server::PacketHeaderSize,
        _packet.size() - Rooftree::Server::PacketHeaderSize);

    // "type" first, it is what a reader of the page's traffic looks for
    boost::json::object json;
    json["type"] = type->Type;
    for (auto& [key, value] : schema.ToJson(*schema.Find(type->Table), payload))
        json.emplace(key, std::move(value));

    _json = boost::json::serialize(json);
    return _json;
}

ServerPacket MakeSystemMessage(std::string_view text)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Arena::CreateSystemMessage(builder, builder.CreateString(text)));
    return { Arena::Opcode::SystemMessage, builder };
}

ClientMessageResult ReadClientPacket(std::string_view packet, std::unique_ptr<ClientMessage>& message)
{
    Optional<Rooftree::Server::PacketHeader> header = Rooftree::Server::ReadPacketHeader(packet);
    if (!header)
        return ClientMessageResult::Invalid;

    PacketType const* type = FindPacketType(ClientPackets, Arena::Opcode(header->Opcode));
    if (!type)
        return ClientMessageResult::Unknown;

    // Copied first: the verifier checks alignment, the packet's own buffer may have none
    message = std::make_unique<ClientMessage>();
    message->Opcode = type->Opcode;
    message->Payload.assign(packet.begin() + Rooftree::Server::PacketHeaderSize, packet.end());
    if (!type->Verify(message->Payload))
    {
        message.reset();
        return ClientMessageResult::Invalid;
    }

    return ClientMessageResult::Ok;
}

ClientMessageResult ReadClientJson(std::string_view text, std::unique_ptr<ClientMessage>& message)
{
    Optional<boost::json::object> json = Rooftree::Server::ParseJsonObject(text);
    boost::json::string const* name = json && json->contains("type") ? json->at("type").if_string() : nullptr;
    if (!name)
        return ClientMessageResult::Invalid;

    auto type = std::ranges::find(ClientPackets, std::string_view(*name), &PacketType::Type);
    if (type == std::end(ClientPackets))
        return ClientMessageResult::Unknown;

    // As the JSON messages always were: a field of the wrong type is as if not sent
    FlatJsonSchema const& schema = GetArenaSchema();
    Rooftree::Server::FlatJsonErrors errors;
    std::string payload = schema.FromJson(*schema.Find(type->Table), *json, errors);

    message = std::make_unique<ClientMessage>();
    message->Opcode = type->Opcode;
    message->Payload.assign(payload.begin(), payload.end());
    return ClientMessageResult::Ok;
}

std::string GetOpcodeName(Arena::Opcode opcode)
{
    // "" for none, a gap between the client's and the server's opcodes too
    if (char const* name = Arena::EnumNameOpcode(opcode); name && *name)
        return name;
    return Rooftree::StringFormat("0x{:04X}", uint32(opcode));
}
