/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The edge between JSON and the operations, which work in FlatBuffers (ApiCall.h): what a route (or
 * the console) was sent becomes the operation's request table, by the schema (FlatJson.h), and the
 * table it answers with becomes the JSON the route sends back. Packets skip it, they are tables.
 *
 * A request table is made of the JSON body, the route's {name} segments and the query (for what the
 * body doesn't have): each by its field's name, the text of a path or query as the field's type. An
 * empty body is {}. A path segment
 * that isn't one (posts/abc for a number) is a 404, as no such thing can be there; a query parameter
 * that isn't one is left out. An upload (the admin's, an account's picture) is its multipart body's parts.
 *
 * What of the JSON doesn't fit goes with the call (ApiCall::GetErrors) as RequestFields would name it:
 * invalid_type for a value of another type, invalid for a number out of range or an unknown key.
 */

#ifndef BLOG_API_JSON_H
#define BLOG_API_JSON_H

#include "Validation.h"
#include <boost/beast/http/status.hpp>
#include <boost/json/value.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rooftree::Server
{
class FlatJsonSchema;
struct PacketError;
}

namespace Blog
{
struct ApiOperation;

struct JsonRequest
{
    std::string Request;                    // the table
    FieldErrors Errors;
    // When there is no table: 400 invalid_json, 400 invalid_upload, 404 not_found
    boost::beast::http::status Status = boost::beast::http::status::ok;
    std::string Code;
};

// target: path and query; path: the route's {name} segments
JsonRequest RequestFromJson(ApiOperation const& operation, std::string_view body, std::string_view contentType, std::string_view target,
    std::vector<std::pair<std::string, std::string>> const& path);

// The answer as JSON: the response table's, or {"error": {...}} of an Error, its details among its
// keys; retryAfter gets the Error's (0 for none)
boost::json::value AnswerToJson(ApiOperation const& operation, boost::beast::http::status status, std::string const& payload,
    uint32* retryAfter = nullptr);

// {"error": {"code": ..., "message": ..., "fields": {...}, <details>}}
boost::json::object ErrorToJson(Rooftree::Server::PacketError const& error);

// Api.fbs (every operation's tables), loaded
Rooftree::Server::FlatJsonSchema const& GetApiSchema();
}

#endif // BLOG_API_JSON_H
