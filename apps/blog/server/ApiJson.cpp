/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "ApiJson.h"
#include "ApiOperations.h"
#include "Api_bfbs_generated.h"
#include "Errors.h"
#include "FlatJson.h"
#include "HttpRequestUtils.h"
#include "JsonHttp.h"
#include "Packet.h"
#include "Packets_generated.h"
#include "StringConvert.h"
#include <algorithm>

namespace http = boost::beast::http;
using Rooftree::Server::FlatJsonSchema;

namespace Blog
{
namespace
{
// The converter's codes as RequestFields has them, its paths without an element's index ("tags.1")
FieldErrors NameErrors(Rooftree::Server::FlatJsonErrors const& errors)
{
    FieldErrors named;
    for (auto [path, code] : errors)
    {
        while (true)
        {
            std::size_t dot = path.rfind('.');
            std::string_view last = std::string_view(path).substr(dot == std::string::npos ? 0 : dot + 1);
            if (dot == std::string::npos || last.empty() || !std::ranges::all_of(last, [](char c) { return c >= '0' && c <= '9'; }))
                break;
            path.resize(dot);
        }
        bool wrongNumber = code == "out_of_range" || code == "not_an_integer" || code == "unknown_value" || code == "unknown_key";
        named.emplace_back(std::move(path), wrongNumber ? "invalid" : "invalid_type");
    }
    return named;
}

// The parts of a multipart upload as an UploadRequest
JsonRequest UploadFromMultipart(std::string_view contentType, std::string_view body, std::vector<std::pair<std::string, std::string>> const& path)
{
    Optional<std::vector<Rooftree::Net::Http::MultipartPart>> parts = Rooftree::Net::Http::ParseMultipartFormData(contentType, body);
    if (!parts)
        return { .Status = http::status::bad_request, .Code = "invalid_upload" };

    auto part = [&](std::string_view name) -> Rooftree::Net::Http::MultipartPart const*
    {
        auto found = std::ranges::find(*parts, name, &Rooftree::Net::Http::MultipartPart::Name);
        return found != parts->end() ? &*found : nullptr;
    };

    flatbuffers::FlatBufferBuilder builder;
    Rooftree::Net::Http::MultipartPart const* file = part("file");
    auto name = file && file->FileName ? builder.CreateString(*file->FileName) : 0;
    auto data = file ? builder.CreateVector(reinterpret_cast<uint8 const*>(file->Data.data()), file->Data.size()) : 0;
    Rooftree::Net::Http::MultipartPart const* thumbhash = part("thumbhash");
    auto hash = thumbhash ? builder.CreateString(thumbhash->Data) : 0;
    auto size = [&](std::string_view key) -> flatbuffers::Optional<uint16>
    {
        Rooftree::Net::Http::MultipartPart const* found = part(key);
        Optional<uint16> value = found ? Rooftree::StringTo<uint16>(found->Data) : Optional<uint16>();
        return value ? flatbuffers::Optional<uint16>(*value) : flatbuffers::nullopt;
    };
    // A post's: its id in the path
    flatbuffers::Optional<uint32> post;
    for (auto const& [key, value] : path)
        if (key == "id")
        {
            Optional<uint32> id = Rooftree::StringTo<uint32>(value);
            if (!id)
                return { .Status = http::status::not_found, .Code = "not_found" };
            post = *id;
        }
    builder.Finish(Admin::CreateUploadRequest(builder, name, data, size("width"), size("height"), hash, post));
    return { .Request = std::string(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize()) };
}

// An account's picture: the multipart's file
JsonRequest AvatarFromMultipart(std::string_view contentType, std::string_view body)
{
    Optional<std::vector<Rooftree::Net::Http::MultipartPart>> parts = Rooftree::Net::Http::ParseMultipartFormData(contentType, body);
    if (!parts)
        return { .Status = http::status::bad_request, .Code = "invalid_upload" };

    flatbuffers::FlatBufferBuilder builder;
    auto file = std::ranges::find(*parts, std::string_view("file"), &Rooftree::Net::Http::MultipartPart::Name);
    auto data = file != parts->end() ? builder.CreateVector(reinterpret_cast<uint8 const*>(file->Data.data()), file->Data.size()) : 0;
    builder.Finish(Reader::CreateAvatarUploadRequest(builder, data));
    return { .Request = std::string(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize()) };
}
}

FlatJsonSchema const& GetApiSchema()
{
    static FlatJsonSchema const schema = []
    {
        FlatJsonSchema loaded(ApiBinarySchema::data());
        std::string error;
        bool ok = loaded.Load(error);
        ASSERT(ok, "Api.fbs: %s", error.c_str());
        return loaded;
    }();
    return schema;
}

JsonRequest RequestFromJson(ApiOperation const& operation, std::string_view body, std::string_view contentType, std::string_view target,
    std::vector<std::pair<std::string, std::string>> const& path)
{
    if (operation.Opcode.Value == uint16(Admin::Opcode::Upload) || operation.Opcode.Value == uint16(Admin::Opcode::PostsUpload))
        return UploadFromMultipart(contentType, body, path);
    if (operation.Opcode.Value == uint16(Reader::Opcode::AvatarUpload))
        return AvatarFromMultipart(contentType, body);

    FlatJsonSchema const& schema = GetApiSchema();
    reflection::Object const& table = *schema.Find(operation.Request);
    auto isPath = [&](std::string_view name) { return std::ranges::find(path, name, &std::pair<std::string, std::string>::first) != path.end(); };
    std::vector<std::string_view> fields = schema.GetFieldNames(table);
    bool hasBody = operation.Method != http::verb::get && operation.Method != http::verb::delete_;

    // No body at all says nothing, like {}: a restore's POST, whose drive may be in the query
    boost::json::object json;
    if (hasBody && !body.empty() && std::ranges::any_of(fields, [&](std::string_view name) { return !isPath(name); }))
    {
        Optional<boost::json::object> parsed = Rooftree::Server::ParseJsonObject(body);
        if (!parsed)
            return { .Status = http::status::bad_request, .Code = "invalid_json" };
        json = std::move(*parsed);
    }

    for (auto const& [name, text] : path)
    {
        Optional<boost::json::value> value = schema.FromText(table, name, text);
        if (!value)
            return { .Status = http::status::not_found, .Code = "not_found" };
        json[name] = std::move(*value);
    }

    // What the body doesn't say: a drive's ?user= on a change too
    for (std::string_view name : fields)
        if (!isPath(name) && !json.contains(name))
            if (Optional<std::string> text = Rooftree::Net::Http::GetQueryParameter(target, name))
                if (Optional<boost::json::value> value = schema.FromText(table, name, *text))
                    json[name] = std::move(*value);

    Rooftree::Server::FlatJsonErrors errors;
    JsonRequest request;
    request.Request = schema.FromJson(table, json, errors);
    request.Errors = NameErrors(errors);
    return request;
}

boost::json::object ErrorToJson(Rooftree::Server::PacketError const& error)
{
    boost::json::object json{ { "code", error.Code.empty() ? std::string_view("error") : std::string_view(error.Code) } };
    if (!error.Message.empty())
        json["message"] = error.Message;
    if (!error.Fields.empty())
    {
        boost::json::object fields;
        for (auto const& [name, code] : error.Fields)
            fields[name] = code;
        json["fields"] = std::move(fields);
    }
    for (Rooftree::Server::PacketError::Detail const& detail : error.Details)
    {
        if (!detail.Values)
        {
            json[detail.Name] = detail.Value;
            continue;
        }
        boost::json::array values;
        for (std::string const& value : *detail.Values)
            values.emplace_back(value);
        json[detail.Name] = std::move(values);
    }
    return boost::json::object{ { "error", std::move(json) } };
}

boost::json::value AnswerToJson(ApiOperation const& operation, http::status status, std::string const& payload, uint32* retryAfter)
{
    std::span<uint8 const> bytes(reinterpret_cast<uint8 const*>(payload.data()), payload.size());
    if (unsigned(status) < 300)
        return GetApiSchema().ToJson(*GetApiSchema().Find(operation.Response), bytes);

    Rooftree::Packets::Error const* table = flatbuffers::GetRoot<Rooftree::Packets::Error>(bytes.data());
    Rooftree::Server::PacketError error{ .Status = table->status(), .Code = table->code() ? table->code()->str() : std::string(),
        .Message = table->message() ? table->message()->str() : std::string(), .RetryAfter = table->retryAfter() };
    if (table->fields())
        for (Rooftree::Packets::ErrorField const* field : *table->fields())
            error.Fields.emplace_back(field->name()->str(), field->code()->str());
    if (table->details())
    {
        for (Rooftree::Packets::ErrorDetail const* detail : *table->details())
        {
            Rooftree::Server::PacketError::Detail& added = error.Details.emplace_back();
            added.Name = detail->name() ? detail->name()->str() : std::string();
            added.Value = detail->value() ? detail->value()->str() : std::string();
            if (detail->values())
            {
                added.Values.emplace();
                for (flatbuffers::String const* value : *detail->values())
                    added.Values->push_back(value->str());
            }
        }
    }
    if (retryAfter)
        *retryAfter = error.RetryAfter;
    return ErrorToJson(error);
}
}
