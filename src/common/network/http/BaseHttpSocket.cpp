/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "BaseHttpSocket.h"
#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http/serializer.hpp>

namespace Rooftree::Net::Http
{
namespace
{
// What serializer gives until done(serializer), copied into one buffer that grows as needed
template<typename Serializer, typename Done>
MessageBuffer Serialize(Serializer& serializer, Done done)
{
    MessageBuffer bytes(4096);
    while (!done(serializer))
    {
        boost::system::error_code error;
        std::size_t taken = 0;
        serializer.next(error, [&](boost::system::error_code&, auto const& buffers)
        {
            std::size_t size = boost::asio::buffer_size(buffers);
            if (bytes.GetRemainingSpace() < size)
                bytes.Resize(bytes.GetBufferSize() + size + 4096);
            for (auto const& buffer : boost::beast::buffers_range_ref(buffers))
                bytes.Write(buffer.data(), buffer.size());
            taken = size;
        });
        if (error)
            break;
        serializer.consume(taken);
    }
    return bytes;
}
}

bool ParseRequest(MessageBuffer& buffer, RequestParser& parser, boost::system::error_code& error)
{
    error = {};
    if (!parser.is_done())
    {
        std::size_t used = parser.put(boost::asio::const_buffer(buffer.GetReadPointer(), buffer.GetActiveSize()), error);
        buffer.ReadCompleted(used);
        // Not an error, there is more to come
        if (error == boost::beast::http::error::need_more)
            error = {};
    }
    return parser.is_done();
}

MessageBuffer SerializeResponse(Request const& request, Response const& response)
{
    boost::beast::http::response_serializer<ResponseBody> serializer(response);
    if (request.method() == boost::beast::http::verb::head)
    {
        serializer.split(true);
        return Serialize(serializer, [](auto& s) { return s.is_header_done(); });
    }
    return Serialize(serializer, [](auto& s) { return s.is_done(); });
}

MessageBuffer SerializeRequest(Request const& request)
{
    boost::beast::http::request_serializer<RequestBody> serializer(request);
    return Serialize(serializer, [](auto& s) { return s.is_done(); });
}

void AbstractSocket::LogExchange(RequestContext const& context, MessageBuffer const& response) const
{
    Logger const* logger = sLog->GetEnabledLogger("server.http", LOG_LEVEL_DEBUG);
    if (!logger)
        return;

    std::string client = GetClientInfo();
    sLog->OutMessageTo(logger, "server.http", LOG_LEVEL_DEBUG, "{} Request {} {} done, status {}", client,
        ToStdStringView(context.request.method_string()), ToStdStringView(context.request.target()), context.response.result_int());
    if (!sLog->ShouldLog("server.http", LOG_LEVEL_TRACE))
        return;

    auto text = [](MessageBuffer const& bytes)
    {
        return std::string_view(reinterpret_cast<char const*>(bytes.GetReadPointer()), bytes.GetActiveSize());
    };
    if (CanLogRequestContent(context))
        sLog->OutMessageTo(logger, "server.http", LOG_LEVEL_TRACE, "{} Request: {}", client, text(SerializeRequest(context.request)));
    else
        sLog->OutMessageTo(logger, "server.http", LOG_LEVEL_TRACE, "{} Request: <REDACTED>", client);
    if (CanLogResponseContent(context))
        sLog->OutMessageTo(logger, "server.http", LOG_LEVEL_TRACE, "{} Response: {}", client, text(response));
    else
        sLog->OutMessageTo(logger, "server.http", LOG_LEVEL_TRACE, "{} Response: <REDACTED>", client);
}
}
