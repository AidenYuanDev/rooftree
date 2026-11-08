/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * JSON bodies for the HTTP API, with Boost.JSON. Errors always have one shape, so a client handles
 * them in one place:
 *
 *   {"error": {"code": "validation_failed", "message": "...", "fields": {"email": "invalid"}}}
 *
 * code is for programs (the frontend translates it), message for people reading logs, fields maps
 * a request field to its own code. Requests are decoded with boost::json::try_value_to, so a struct
 * described with BOOST_DESCRIBE_STRUCT is filled field by field.
 */

#ifndef ROOFTREE_JSON_HTTP_H
#define ROOFTREE_JSON_HTTP_H

#include "Define.h"
#include "HttpCommon.h"
#include "Optional.h"
#include <boost/json/object.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>
#include <boost/json/value_to.hpp>
#include <string_view>

namespace Rooftree::Server
{
void SetJsonResponse(Net::Http::Response& response, boost::beast::http::status status, boost::json::value const& body);

void SetJsonError(Net::Http::Response& response, boost::beast::http::status status, std::string_view code,
    std::string_view message = {}, boost::json::object fields = {});

// Empty if the body is not JSON or not an object
Optional<boost::json::object> ParseJsonObject(std::string_view body);

// The body decoded as T (a described struct, a map, ...). Empty if it is not JSON or doesn't convert
template<typename T>
Optional<T> ParseJsonBody(std::string_view body)
{
    boost::system::error_code error;
    boost::json::value value = boost::json::parse(body, error);
    if (error)
        return {};

    boost::system::result<T> result = boost::json::try_value_to<T>(value);
    if (!result)
        return {};

    return std::move(*result);
}

// Typed reads of an object member, empty if missing or of another type
Optional<std::string_view> GetJsonString(boost::json::object const& object, std::string_view key);
Optional<int64> GetJsonInt(boost::json::object const& object, std::string_view key);
Optional<bool> GetJsonBool(boost::json::object const& object, std::string_view key);
}

#endif // ROOFTREE_JSON_HTTP_H
