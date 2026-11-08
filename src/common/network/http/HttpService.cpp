/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "HttpService.h"
#include "BaseHttpSocket.h"
#include "HttpRequestUtils.h"
#include <boost/beast/version.hpp>
#include <ctime>

namespace Rooftree::Net::Http
{
namespace http = boost::beast::http;

bool CanLogRequestContent(RequestContext const& context)
{
    return !context.handler || !HasFlag(context.handler->Flags, RequestHandlerFlag::DoNotLogRequestContent);
}

bool CanLogResponseContent(RequestContext const& context)
{
    return !context.handler || !HasFlag(context.handler->Flags, RequestHandlerFlag::DoNotLogResponseContent);
}

namespace
{
// RFC 9110's date: Sat, 07 Nov 2026 12:31:48 GMT
std::string HttpDate()
{
    time_t now = time(nullptr);
    tm utc;
    gmtime_r(&now, &utc);
    char text[64];
    std::strftime(text, sizeof(text), "%a, %d %b %Y %T GMT", &utc);
    return text;
}
}

RequestHandlerResult DispatcherService::HandleRequest(std::shared_ptr<AbstractSocket> socket, RequestContext& context)
{
    RT_LOG_DEBUG(_logger, "{} Starting request {} {}", socket->GetClientInfo(), ToStdStringView(context.request.method_string()),
        ToStdStringView(context.request.target()));

    context.response.set(http::field::date, HttpDate());
    context.response.set(http::field::server, BOOST_BEAST_VERSION_STRING);
    context.response.keep_alive(context.request.keep_alive());
    for (auto const& [name, value] : _defaultHeaders)
        context.response.set(name, value);

    Optional<std::vector<std::string>> segments = DecodePathSegments(ToStdStringView(context.request.target()));
    if (!segments)
    {
        context.response.result(http::status::bad_request);
        return RequestHandlerResult::Handled;
    }

    std::string allowed;
    switch (_router.Find(context.request.method(), *segments, context.handler, context.pathParameters, &allowed))
    {
        case RouteMatchResult::Found:
            return context.handler->Func(std::move(socket), context);
        case RouteMatchResult::MethodNotAllowed:
            context.response.result(http::status::method_not_allowed);
            context.response.set(http::field::allow, allowed);
            return RequestHandlerResult::Handled;
        default:
            break;
    }

    if (_fallback)
    {
        context.handler = &*_fallback;
        return _fallback->Func(std::move(socket), context);
    }
    context.response.result(http::status::not_found);
    return RequestHandlerResult::Handled;
}

void DispatcherService::RegisterHandler(http::verb method, std::string_view path, HandlerFunction handler, RequestHandlerFlag flags)
{
    std::string_view methodName = ToStdStringView(http::to_string(method));
    if (!_router.Add(method, path, { .Func = std::move(handler), .Flags = flags }))
        ABORT_MSG("A second route, or an invalid one, for %.*s %.*s", int(methodName.size()), methodName.data(), int(path.size()), path.data());
    RT_LOG_INFO(_logger, "Registered new handler for {} {}", methodName, path);
}
}
