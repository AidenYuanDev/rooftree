/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "JsonHttp.h"
#include <boost/json/serialize.hpp>

namespace Rooftree::Server
{
void SetJsonResponse(Net::Http::Response& response, boost::beast::http::status status, boost::json::value const& body)
{
    response.result(status);
    response.set(boost::beast::http::field::content_type, "application/json; charset=utf-8");
    // API responses depend on the session cookie, no shared cache may keep them
    response.set(boost::beast::http::field::cache_control, "no-store");
    response.body() = boost::json::serialize(body);
}

void SetJsonError(Net::Http::Response& response, boost::beast::http::status status, std::string_view code,
    std::string_view message, boost::json::object fields)
{
    boost::json::object error{ { "code", code } };
    if (!message.empty())
        error["message"] = message;
    if (!fields.empty())
        error["fields"] = std::move(fields);

    SetJsonResponse(response, status, boost::json::object{ { "error", std::move(error) } });
}

Optional<boost::json::object> ParseJsonObject(std::string_view body)
{
    boost::system::error_code error;
    boost::json::value value = boost::json::parse(body, error);
    if (error || !value.is_object())
        return {};

    return std::move(value.as_object());
}

Optional<std::string_view> GetJsonString(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    if (!value || !value->is_string())
        return {};

    return std::string_view(value->get_string());
}

Optional<int64> GetJsonInt(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    if (!value)
        return {};

    if (value->is_int64())
        return value->get_int64();
    if (value->is_uint64() && value->get_uint64() <= uint64(std::numeric_limits<int64>::max()))
        return int64(value->get_uint64());

    return {};
}

Optional<bool> GetJsonBool(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    if (!value || !value->is_bool())
        return {};

    return value->get_bool();
}
}
