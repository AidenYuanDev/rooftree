/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The request and the response of a connection's exchange, as Boost.Beast has them, and what a route
 * handler answers with.
 */

#ifndef ROOFTREE_HTTP_COMMON_H
#define ROOFTREE_HTTP_COMMON_H

#include "Define.h"
#include "HttpRouter.h"
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/string_body.hpp>
#include <istream>
#include <memory>

namespace Rooftree::Net::Http
{
using RequestBody = boost::beast::http::string_body;
using ResponseBody = boost::beast::http::string_body;
using Request = boost::beast::http::request<RequestBody>;
using Response = boost::beast::http::response<ResponseBody>;

// What a route's log shows of it
enum class RequestHandlerFlag : uint8
{
    None                    = 0x0,
    DoNotLogRequestContent  = 0x1,      // passwords, uploads
    DoNotLogResponseContent = 0x2,      // tokens, files
};

constexpr RequestHandlerFlag operator|(RequestHandlerFlag a, RequestHandlerFlag b) { return RequestHandlerFlag(uint8(a) | uint8(b)); }
constexpr bool HasFlag(RequestHandlerFlag flags, RequestHandlerFlag flag) { return (uint8(flags) & uint8(flag)) != 0; }

// A body read from a stream (a file) as the connection takes it, a piece at a time, so a download of
// gigabytes isn't held in memory: Length bytes from where Stream is. Content-Length is Length
struct ResponseStream
{
    std::shared_ptr<std::istream> Stream;
    std::uint64_t Length = 0;
};

struct RequestHandler;

struct RequestContext
{
    Request request;
    Response response;
    RequestHandler const* handler = nullptr;    // the route's, once found
    PathParameters pathParameters;              // its {name} segments
    std::shared_ptr<ResponseStream> responseStream; // set, the body comes from it instead of response.body()
};

bool CanLogRequestContent(RequestContext const& context);
bool CanLogResponseContent(RequestContext const& context);

inline std::string_view ToStdStringView(boost::beast::string_view text) { return { text.data(), text.size() }; }

enum class RequestHandlerResult
{
    Handled,        // the response is ready
    Error,          // answered too, then the connection closes
    Async,          // answered later, through the socket's SendResponse on its network thread
};
}

#endif // ROOFTREE_HTTP_COMMON_H
