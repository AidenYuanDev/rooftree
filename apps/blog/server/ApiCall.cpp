/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "ApiCall.h"
#include "ApiOperations.h"
#include "Errors.h"
#include "Log.h"
#include "Packet.h"

namespace http = boost::beast::http;

namespace Blog
{
ApiCall::~ApiCall()
{
    if (!_responded)
    {
        RT_LOG_ERROR("blog", "{} by {} was never answered, sending 500", GetOpcodeName(_params.Opcode),
            _params.Auth ? _params.Auth->User.Username : _params.ClientAddress.to_string());
        RespondError(http::status::internal_server_error, "internal_error");
    }
}

AuthInfo const& ApiCall::GetAuth() const
{
    ASSERT(_params.Auth, "%s: an operation without a login asked for it", std::string(GetOpcodeName(_params.Opcode)).c_str());
    return *_params.Auth;
}

void ApiCall::Answer(http::status status, std::string payload)
{
    if (_responded.exchange(true))
        return;
    _done(status, std::move(payload), _login);
}

void ApiCall::Respond(flatbuffers::FlatBufferBuilder const& builder, http::status status)
{
    Answer(status, Bytes(builder));
}

void ApiCall::Respond(std::string payload, http::status status)
{
    Answer(status, std::move(payload));
}

void ApiCall::RespondError(http::status status, std::string_view code, std::string_view message, boost::json::object const& fields)
{
    Rooftree::Server::PacketError error{ .Status = uint16(status), .Code = std::string(code), .Message = std::string(message) };
    for (auto const& [name, fieldCode] : fields)
        error.Fields.emplace_back(name, fieldCode.is_string() ? std::string(fieldCode.get_string()) : std::string());
    RespondError(error);
}

void ApiCall::RespondError(Rooftree::Server::PacketError const& error)
{
    Answer(http::status(error.Status), Rooftree::Server::MakeErrorPayload(_params.Opcode, error));
}

void RespondValidation(ApiRequest const& request, boost::json::object fields)
{
    request->RespondError(http::status::bad_request, "validation_failed", {}, fields);
}

void RespondNotFound(ApiRequest const& request)
{
    request->RespondError(http::status::not_found, "not_found");
}

void RespondInternalError(ApiRequest const& request)
{
    request->RespondError(http::status::internal_server_error, "internal_error");
}

Rooftree::Server::PacketError BannedError(UserRow const& user)
{
    Rooftree::Server::PacketError error{ .Status = uint16(http::status::forbidden), .Code = "banned",
        .Details = { { .Name = "until", .Value = FormatTime(*user.BannedUntil) } } };
    if (!user.BanReason.empty())
        error.Details.push_back({ .Name = "reason", .Value = user.BanReason });
    return error;
}

void RespondBanned(ApiRequest const& request, UserRow const& user)
{
    request->RespondError(BannedError(user));
}

void RespondNotInLang(ApiRequest const& request, std::vector<std::string> const& langs)
{
    request->RespondError({ .Status = uint16(http::status::not_found), .Code = "not_in_lang", .Details = { { .Name = "translations", .Values = langs } } });
}

std::string ReadLang(flatbuffers::String const* lang)
{
    return lang && lang->string_view() == "en" ? "en" : "zh";
}

std::string ReadText(flatbuffers::String const* text)
{
    return text ? text->str() : std::string();
}

void RespondOk(ApiRequest const& request)
{
    flatbuffers::FlatBufferBuilder builder;
    if (*Reader::EnumNameOpcode(Reader::Opcode(request->GetOpcode())))
        builder.Finish(Reader::CreateOk(builder, true));
    else
        builder.Finish(Admin::CreateOk(builder, true));
    request->Respond(builder);
}

std::string Bytes(flatbuffers::FlatBufferBuilder const& builder)
{
    return std::string(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize());
}

flatbuffers::Offset<flatbuffers::String> TimeString(flatbuffers::FlatBufferBuilder& builder, Optional<SystemTimePoint> time)
{
    return time ? builder.CreateString(FormatTime(*time)) : 0;
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> StringList(flatbuffers::FlatBufferBuilder& builder,
    std::vector<std::string> const& values)
{
    return builder.CreateVectorOfStrings(values);
}

bool IsEmptyTable(flatbuffers::Table const* table)
{
    // Nothing set: every entry of its vtable after the two sizes is 0
    if (!table)
        return true;
    uint8 const* vtable = table->GetVTable();
    for (flatbuffers::voffset_t offset = 4; offset < flatbuffers::ReadScalar<flatbuffers::voffset_t>(vtable); offset += 2)
        if (flatbuffers::ReadScalar<flatbuffers::voffset_t>(vtable + offset))
            return false;
    return true;
}
}
