/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "BlogWebSocket.h"
#include "ApiPackets.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "BlogDatabase.h"
#include "Collab.h"
#include "Field.h"
#include "PostAccess.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "SecurityUtils.h"
#include "WorldHttpService.h"

namespace Blog
{
namespace
{
constexpr std::size_t MaxPathLength = 300;
// A page's watch list: "post:12", "guestbook", a few of them
constexpr std::size_t MaxWatchTargets = 16;
constexpr std::size_t MaxWatchTargetLength = 40;
// GenerateToken(32): 43 characters of base64url, like BlogSession.cpp's
constexpr std::size_t SessionTokenLength = 43;
}

void RegisterWebSocketRoute(Rooftree::Server::WorldHttpService& service, Rooftree::Server::WebSocketRouter const& router,
    std::vector<std::string> allowedOrigins, std::string cookieName)
{
    std::size_t maxLoggedInMessageSize = service.GetSettings().MaxRequestBodySize;
    service.WebSocketRoute("/ws", [&router, allowedOrigins = std::move(allowedOrigins), cookieName = std::move(cookieName), maxLoggedInMessageSize](
        Rooftree::Net::Http::Request const& request,
        Rooftree::Net::Http::Response& response, boost::asio::ip::address const& client) -> std::shared_ptr<Rooftree::Net::WebSocket::Connection>
    {
        // A page of ours, not another site opening sockets in its readers' browsers
        if (!Rooftree::Server::CheckOrigin(request, allowedOrigins))
        {
            RT_LOG_DEBUG("blog.ws", "{}: refused, Origin {} is not ours", client.to_string(), Rooftree::Server::GetRequestOrigin(request));
            response.result(boost::beast::http::status::forbidden);
            return nullptr;
        }

        std::string loginHash;
        if (Optional<std::string_view> cookie = Rooftree::Net::Http::GetCookie(request, cookieName); cookie && cookie->size() == SessionTokenLength)
            loginHash = Rooftree::Server::Sha256Hex(*cookie);

        std::shared_ptr<BlogWebSocket> socket = std::make_shared<BlogWebSocket>(router, client,
            Rooftree::Server::WorldHttpService::AddressSessionKey(client), std::move(loginHash),
            std::string(request[boost::beast::http::field::user_agent]));
        // Pages send a path, nothing big; a login may send packets (ApiPackets.h) as big as a request
        socket->SetMaxMessageSize(socket->GetLoginHash().empty() ? 4 * 1024 : maxLoggedInMessageSize);
        return socket;
    });
}

void BlogSession::HandleWsPage(Rooftree::Server::WebSocketJsonMessage& message)
{
    boost::json::value const* path = message.GetBody().if_contains("path");
    if (!path || !path->is_string() || path->as_string().empty() || path->as_string()[0] != '/' || path->as_string().size() > MaxPathLength)
    {
        message.Reply("error", { { "code", "invalid_path" } });
        return;
    }

    // Without the query and the fragment: /zh/search?q=x and /zh/search are one page
    std::string page(path->as_string());
    if (std::size_t end = page.find_first_of("?#"); end != std::string::npos)
        page.resize(end);
    // What it shows that may change while it's open ("post:12"): the first few, the others ignored
    std::vector<std::string> watch;
    if (boost::json::value const* targets = message.GetBody().if_contains("watch"); targets && targets->is_array())
        for (boost::json::value const& target : targets->as_array())
            if (target.is_string() && !target.as_string().empty() && target.as_string().size() <= MaxWatchTargetLength
                && watch.size() < MaxWatchTargets)
                watch.emplace_back(target.as_string());
    // Who reads it: the login of the handshake's cookie, while it lasts
    std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = message.GetSocket();
    std::string const& loginHash = static_cast<BlogWebSocket const&>(*socket).GetLoginHash();
    if (loginHash.empty())
        return sWorld->GetPresence().Visit(socket, std::move(page), 0, std::move(watch));

    FindLogin(loginHash, [socket, page = std::move(page), watch = std::move(watch)](AuthInfo const* auth) mutable
    {
        sWorld->GetPresence().Visit(socket, std::move(page), auth ? auth->User.Id : 0, std::move(watch));
    });
}

namespace
{
// Loaded only if it isn't open yet; if another page opened it meanwhile, its text stays
Rooftree::Task<void> OpenDoc(BlogSession& session, std::shared_ptr<Rooftree::Server::WorldWebSocket> socket, DocKey key, UserRow user,
    DocPeer peer, Optional<uint32> sinceVersion)
{
    struct Result
    {
        bool Allowed = false;
        Optional<DocText> Text;
    };

    Result result = co_await session.OnWorker([key, user, load = !sWorld->GetCollab().IsLoaded(key)]
    {
        Result result;
        Optional<PostAccess> access = WorkerPostAccess(key.PostId, user);
        result.Allowed = access && access->CanEdit();
        if (!result.Allowed || !load)
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_POST_DOC_TEXTS);
        stmt->setString(0, key.Lang);
        stmt->setString(1, key.Lang);
        stmt->setUInt32(2, key.PostId);
        DocText text;
        if (PreparedQueryResult row = BlogDatabase.Query(stmt))
        {
            Field* fields = row->Fetch();
            std::size_t at = key.Field == "title" ? 0 : key.Field == "summary" ? 2 : 4;
            text.Text = fields[at].GetStringOrNull().value_or("");
            text.Version = fields[at + 1].GetUInt32OrNull().value_or(0);
        }
        result.Text = std::move(text);
        return result;
    });
    if (!result.Allowed)
        co_return socket->SendJson("doc", { { "op", "error" }, { "doc", key.Name() }, { "code", "not_found" } });
    sWorld->GetCollab().Open(key, socket, std::move(peer), sinceVersion, std::move(result.Text));
}
}

void BlogSession::HandleWsDoc(Rooftree::Server::WebSocketJsonMessage& message)
{
    boost::json::object const& body = message.GetBody();
    boost::json::value const* name = body.if_contains("doc");
    boost::json::value const* op = body.if_contains("op");
    Optional<DocKey> key = name && name->is_string() ? DocKey::Parse(name->as_string()) : Optional<DocKey>();
    if (!key || !op || !op->is_string())
        return message.Reply("doc", { { "op", "error" }, { "code", "invalid" } });

    std::shared_ptr<Rooftree::Server::WorldWebSocket> socket = message.GetSocket();
    Collab& collab = sWorld->GetCollab();
    std::string_view action = op->as_string();
    // Only a page that opened it, which the open checked, gets this far in the others
    if (action == "push")
    {
        boost::json::value const* version = body.if_contains("version");
        boost::json::value const* updates = body.if_contains("updates");
        if (!version || !version->is_number() || !updates)
            return message.Reply("doc", { { "op", "error" }, { "doc", key->Name() }, { "code", "invalid" } });
        return collab.Push(*key, socket, uint32(version->to_number<double>()), *updates);
    }
    if (action == "cursor")
    {
        boost::json::value const* selection = body.if_contains("selection");
        return collab.Cursor(*key, socket, selection ? *selection : boost::json::value());
    }
    if (action == "close")
        return collab.Close(*key, socket.get());
    if (action != "open")
        return message.Reply("doc", { { "op", "error" }, { "doc", key->Name() }, { "code", "invalid" } });

    boost::json::value const* client = body.if_contains("client");
    boost::json::value const* since = body.if_contains("since");
    std::string loginHash = static_cast<BlogWebSocket const&>(*socket).GetLoginHash();
    if (loginHash.empty() || !client || !client->is_string())
        return message.Reply("doc", { { "op", "error" }, { "doc", key->Name() }, { "code", loginHash.empty() ? "login" : "invalid" } });

    Optional<uint32> sinceVersion = since && since->is_number() ? Optional<uint32>(uint32(since->to_number<double>())) : Optional<uint32>();
    FindLogin(loginHash, [this, socket, key = *key, clientId = std::string(client->as_string()), sinceVersion](AuthInfo const* auth)
    {
        if (!auth)
            return socket->SendJson("doc", { { "op", "error" }, { "doc", key.Name() }, { "code", "login" } });

        Spawn(OpenDoc(*this, socket, key, auth->User, DocPeer{ .Client = clientId, .UserId = auth->User.Id, .Username = auth->User.Username,
            .DisplayName = auth->User.DisplayName, .Avatar = auth->User.Avatar }, sinceVersion));
    });
}

void RegisterWebSocketRoutes(Rooftree::Server::WebSocketRouter& router)
{
    router.Add("page", &BlogSession::HandleWsPage);
    router.Add("doc", &BlogSession::HandleWsDoc);
    RegisterApiPackets(router);
}
}
