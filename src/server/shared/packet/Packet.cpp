/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Packet.h"
#include "Packets_generated.h"

namespace Rooftree::Server
{
Optional<PacketHeader> ReadPacketHeader(std::string_view data)
{
    if (data.size() < PacketHeaderSize)
        return {};

    auto byte = [&](std::size_t i) { return uint32(uint8(data[i])); };
    return PacketHeader{
        .Opcode = uint16(byte(0) | byte(1) << 8),
        .Flags = uint16(byte(2) | byte(3) << 8),
        .RequestId = byte(4) | byte(5) << 8 | byte(6) << 16 | byte(7) << 24,
    };
}

std::string WritePacket(PacketHeader header, std::span<uint8 const> payload)
{
    std::string packet;
    packet.reserve(PacketHeaderSize + payload.size());
    packet += char(header.Opcode & 0xFF);
    packet += char(header.Opcode >> 8);
    packet += char(header.Flags & 0xFF);
    packet += char(header.Flags >> 8);
    for (int shift = 0; shift < 32; shift += 8)
        packet += char((header.RequestId >> shift) & 0xFF);
    packet.append(reinterpret_cast<char const*>(payload.data()), payload.size());
    return packet;
}

std::string MakeErrorPayload(uint16 opcode, PacketError const& error)
{
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Packets::ErrorField>> fields;
    for (auto const& [name, code] : error.Fields)
        fields.push_back(Packets::CreateErrorField(builder, builder.CreateString(name), builder.CreateString(code)));

    std::vector<flatbuffers::Offset<Packets::ErrorDetail>> details;
    for (PacketError::Detail const& detail : error.Details)
        details.push_back(Packets::CreateErrorDetail(builder, builder.CreateString(detail.Name),
            detail.Values ? 0 : builder.CreateString(detail.Value), detail.Values ? builder.CreateVectorOfStrings(*detail.Values) : 0));

    builder.Finish(Packets::CreateError(builder, error.Status, builder.CreateString(error.Code),
        error.Message.empty() ? 0 : builder.CreateString(error.Message), fields.empty() ? 0 : builder.CreateVector(fields), opcode,
        error.RetryAfter, details.empty() ? 0 : builder.CreateVector(details)));
    return std::string(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize());
}

std::string MakeErrorPayload(uint16 opcode, uint16 status, std::string_view code, std::string_view message,
    std::vector<std::pair<std::string, std::string>> const& fields)
{
    return MakeErrorPayload(opcode, PacketError{ .Status = status, .Code = std::string(code), .Message = std::string(message), .Fields = fields });
}

std::string WriteErrorPacket(uint32 requestId, uint16 opcode, uint16 status, std::string_view code, std::string_view message,
    std::vector<std::pair<std::string, std::string>> const& fields)
{
    std::string payload = MakeErrorPayload(opcode, status, code, message, fields);
    return WritePacket({ .Opcode = ErrorOpcode, .RequestId = requestId }, { reinterpret_cast<uint8 const*>(payload.data()), payload.size() });
}
}
