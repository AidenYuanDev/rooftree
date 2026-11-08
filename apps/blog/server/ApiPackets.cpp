/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "ApiPackets.h"
#include "ApiOperations.h"
#include "ApiJson.h"
#include "BlogSession.h"
#include "BlogWebSocket.h"
#include "Errors.h"
#include "FlatJson.h"
#include "Log.h"
#include "Packets_generated.h"
#include "WorldWebSocket.h"

namespace http = boost::beast::http;
using Rooftree::Server::FlatJsonSchema;
using Rooftree::Server::WebSocketPacketMessage;

namespace Blog
{
namespace
{
// Where an answer goes once the operation is done, from any thread
struct PacketReply
{
    std::shared_ptr<Rooftree::Server::WorldWebSocket> Socket;
    Rooftree::Server::PacketHeader Header;

    // The response table, or an Error
    void Send(http::status status, std::string_view payload) const
    {
        Rooftree::Server::PacketHeader header = Header;
        if (unsigned(status) >= 300)
            header.Opcode = Rooftree::Server::ErrorOpcode;
        Socket->SendPacket(header, { reinterpret_cast<uint8 const*>(payload.data()), payload.size() });
    }

    void Error(Rooftree::Server::PacketError const& error) const
    {
        Send(http::status(error.Status), Rooftree::Server::MakeErrorPayload(Header.Opcode, error));
    }
};

// Why auth may not call operation, none if it may
Optional<Rooftree::Server::PacketError> CheckAccess(ApiOperation const& operation, AuthInfo const* auth)
{
    if (operation.Access == ApiAccess::Anyone)
        return {};
    if (!auth)
        return Rooftree::Server::PacketError{ .Status = 401, .Code = "unauthenticated" };
    if (auth->User.IsBanned() && operation.Access != ApiAccess::UserEvenBanned)
        return BannedError(auth->User);
    if ((operation.Access == ApiAccess::Moderator && !auth->User.CanModerate()) || (operation.Access == ApiAccess::Admin && !auth->User.IsAdmin()))
        return Rooftree::Server::PacketError{ .Status = 403, .Code = "forbidden" };
    return {};
}

void HandleApiPacket(Rooftree::Server::Session& session, WebSocketPacketMessage& message, ApiOperation const& operation)
{
    PacketReply reply{ .Socket = message.GetSocket(), .Header = { .Opcode = message.GetOpcode(), .RequestId = message.GetRequestId() } };
    BlogWebSocket const& socket = static_cast<BlogWebSocket const&>(*message.GetSocket());

    // The table goes with the call, the message ends with this handler
    ApiCall::Params params{ .Opcode = operation.Opcode.Value,
        .Request = std::string(reinterpret_cast<char const*>(message.GetPayload().data()), message.GetPayload().size()),
        .ClientAddress = message.GetRemoteAddress(), .UserAgent = socket.GetUserAgent() };

    BlogSession& blog = static_cast<BlogSession&>(session);
    BlogSession::AuthHandler run = [&blog, &operation, reply, params = std::move(params)](AuthInfo const* auth) mutable
    {
        if (Optional<Rooftree::Server::PacketError> error = CheckAccess(operation, auth))
            return reply.Error(*error);

        RT_LOG_DEBUG("blog.packets", "{} {} by {}", params.ClientAddress.to_string(), GetOpcodeName(operation.Opcode.Value),
            auth ? auth->User.Username : "anyone");
        if (auth)
            params.Auth = *auth;
        blog.Spawn(operation.Op(blog, std::make_shared<ApiCall>(std::move(params), [reply](http::status status, std::string payload, LoginChange const& login)
        {
            // The connection's next packets call as nobody, or as the new session
            BlogWebSocket& socket = static_cast<BlogWebSocket&>(*reply.Socket);
            if (login.Ended)
                socket.SetLoginHash({});
            else if (!login.NewHash.empty())
                socket.SetLoginHash(login.NewHash);
            reply.Send(status, payload);
        })));
    };

    // Who calls is the login of the connection's handshake
    if (socket.GetLoginHash().empty())
        return run(nullptr);
    blog.FindLogin(socket.GetLoginHash(), std::move(run));
}
}

void RegisterApiPackets(Rooftree::Server::WebSocketRouter& router)
{
    FlatJsonSchema const& schema = GetApiSchema();
    for (ApiOperation const& operation : GetApiOperations())
    {
        reflection::Object const* request = schema.Find(operation.Request);
        std::string name(GetOpcodeName(operation.Opcode.Value));
        ASSERT(request, "%s: no table %s in Api.fbs", name.c_str(), std::string(operation.Request).c_str());
        ASSERT(schema.Find(operation.Response), "%s: no table %s in Api.fbs", name.c_str(), std::string(operation.Response).c_str());

        router.AddPacket(operation.Opcode.Value, operation.MessageType, [&schema, request](std::span<uint8 const> payload)
        {
            return schema.Verify(*request, payload);
        }, [&operation](Rooftree::Server::Session& session, WebSocketPacketMessage& message)
        {
            HandleApiPacket(session, message, operation);
        });
    }
}
}
