/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "AuthService.h"
#include "Argon2Hash.h"
#include "ArenaDatabase.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "TicketMgr.h"
#include "Transaction.h"
#include "Util.h"
#include <boost/asio/post.hpp>
#include <boost/json/array.hpp>
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <fstream>
#include <sstream>

namespace http = boost::beast::http;
using Rooftree::Net::Http::RequestHandlerFlag;

namespace
{
constexpr uint32 LeaderboardSize = 10;

void SetJsonResponse(Rooftree::Net::Http::RequestContext& context, http::status status, boost::json::object const& body)
{
    context.response.result(status);
    context.response.set(http::field::content_type, "application/json");
    context.response.body() = boost::json::serialize(body);
}

void SetError(Rooftree::Net::Http::RequestContext& context, http::status status, std::string_view error)
{
    SetJsonResponse(context, status, { { "error", error } });
}

// username and password of the request body, false (with the error response set) if they are missing
bool ReadCredentials(Rooftree::Net::Http::RequestContext& context, std::string& username, std::string& password)
{
    boost::system::error_code error;
    boost::json::value body = boost::json::parse(context.request.body(), error);
    boost::json::object const* object = error ? nullptr : body.if_object();
    boost::json::value const* name = object ? object->if_contains("username") : nullptr;
    boost::json::value const* pass = object ? object->if_contains("password") : nullptr;
    if (!name || !name->is_string() || !pass || !pass->is_string())
    {
        SetError(context, http::status::bad_request, "expected {\"username\": ..., \"password\": ...}");
        return false;
    }

    username = name->get_string();
    password = pass->get_string();
    return true;
}

bool IsValidUsername(std::string_view name)
{
    return name.size() >= 3 && name.size() <= 16 && std::ranges::all_of(name, [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool IsValidPassword(std::string_view password)
{
    return password.size() >= 6 && password.size() <= 128;
}

struct AccountRow
{
    uint32 Id;
    std::string Name;           // as stored: the lookup ignores case
    std::string PasswordHash;
};

Optional<AccountRow> FindAccount(std::string const& username)
{
    ArenaDatabasePreparedStatement* stmt = ArenaDatabase.GetPreparedStatement(ARENA_SEL_ACCOUNT_BY_NAME);
    stmt->setString(0, username);
    PreparedQueryResult result = ArenaDatabase.Query(stmt);
    if (!result)
        return {};

    Field* fields = result->Fetch();
    return AccountRow{ .Id = fields[0].GetUInt32(), .Name = fields[1].GetString(), .PasswordHash = fields[2].GetString() };
}
}

AuthService& AuthService::Instance()
{
    static AuthService instance;
    return instance;
}

AuthService::AuthService() : HttpService("arena")
{
    RegisterHandler(http::verb::get, "/", [this](std::shared_ptr<ArenaHttpSession> session, RequestContext& context)
    {
        return HandleGetIndex(std::move(session), context);
    });

    // Passwords and tickets stay out of the trace log
    RegisterHandler(http::verb::post, "/api/register", [this](std::shared_ptr<ArenaHttpSession> session, RequestContext& context)
    {
        return RunAsync(std::move(session), context, [this](RequestContext& ctx) { HandleRegister(ctx); });
    }, RequestHandlerFlag::DoNotLogRequestContent);

    RegisterHandler(http::verb::post, "/api/login", [this](std::shared_ptr<ArenaHttpSession> session, RequestContext& context)
    {
        return RunAsync(std::move(session), context, [this](RequestContext& ctx) { HandleLogin(ctx); });
    }, RequestHandlerFlag::DoNotLogRequestContent | RequestHandlerFlag::DoNotLogResponseContent);

    RegisterHandler(http::verb::get, "/api/leaderboard", [this](std::shared_ptr<ArenaHttpSession> session, RequestContext& context)
    {
        return RunAsync(std::move(session), context, [this](RequestContext& ctx) { HandleGetLeaderboard(ctx); });
    });
}

bool AuthService::Start(Rooftree::Asio::IoContext& ioContext, std::string const& bindIp, uint16 port, int32 threadCount,
    AuthSettings settings, std::size_t workerThreads)
{
    _settings = std::move(settings);
    _workers = std::make_unique<Rooftree::ThreadPool>(workerThreads);
    return StartNetwork(ioContext, bindIp, port, threadCount);
}

void AuthService::Stop()
{
    // Workers first: a running one posts its response to a network thread's io_context, which must
    // still exist then (posting to a stopped one is fine, the handler is just dropped)
    _workers->Stop();
    _workers->Join();
    _workers = nullptr;

    StopNetwork();
}

AuthService::RequestHandlerResult AuthService::RunAsync(std::shared_ptr<ArenaHttpSession> session, RequestContext& context,
    std::function<void(RequestContext&)> work)
{
    // BaseSocket::HandleMessage's context dies when the handler returns, keep our own
    std::shared_ptr<RequestContext> asyncContext = std::make_shared<RequestContext>(std::move(context));
    _workers->PostWork([session = std::move(session), asyncContext, work = std::move(work)]() mutable
    {
        work(*asyncContext);

        // Socket::QueuePacket isn't thread safe, queue the response on the connection's own thread
        auto executor = session->underlying_stream().get_executor();
        boost::asio::post(executor, [session = std::move(session), asyncContext = std::move(asyncContext)]
        {
            session->SendResponse(*asyncContext);
        });
    });

    return RequestHandlerResult::Async;
}

AuthService::RequestHandlerResult AuthService::HandleGetIndex(std::shared_ptr<ArenaHttpSession> /*session*/, RequestContext& context)
{
    // Read on every request so edits show up without a restart. Small file, small cost
    std::ifstream file(_settings.WebRoot + "/index.html", std::ios::binary);
    if (!file)
    {
        RT_LOG_ERROR("arena.http", "Can't open {}/index.html, check Arena.WebRoot", _settings.WebRoot);
        context.response.result(http::status::not_found);
        return RequestHandlerResult::Handled;
    }

    std::ostringstream content;
    content << file.rdbuf();
    context.response.result(http::status::ok);
    context.response.set(http::field::content_type, "text/html; charset=utf-8");
    context.response.set(http::field::cache_control, "no-cache");
    context.response.body() = std::move(content).str();
    return RequestHandlerResult::Handled;
}

void AuthService::HandleRegister(RequestContext& context)
{
    std::string username, password;
    if (!ReadCredentials(context, username, password))
        return;

    if (!IsValidUsername(username))
        return SetError(context, http::status::bad_request, "username: 3 to 16 letters, digits or _");

    if (!IsValidPassword(password))
        return SetError(context, http::status::bad_request, "password: 6 to 128 characters");

    if (FindAccount(username))
        return SetError(context, http::status::conflict, "username is taken");
    Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash(password, _settings.Argon2Iterations, _settings.Argon2MemoryCost);
    if (!hash)
        return SetError(context, http::status::internal_server_error, "could not hash the password");

    ArenaDatabaseTransaction trans = ArenaDatabase.BeginTransaction();
    ArenaDatabasePreparedStatement* stmt = ArenaDatabase.GetPreparedStatement(ARENA_INS_ACCOUNT);
    stmt->setString(0, username);
    stmt->setString(1, *hash);
    trans->Append(stmt);

    stmt = ArenaDatabase.GetPreparedStatement(ARENA_INS_PLAYER);
    stmt->setString(0, username);
    trans->Append(stmt);
    ArenaDatabase.DirectCommitTransaction(trans);

    // Two registrations of one name at once: the unique key let only one INSERT through, the hash
    // tells whose it was
    Optional<AccountRow> account = FindAccount(username);
    if (!account || account->PasswordHash != *hash)
        return SetError(context, http::status::conflict, "username is taken");

    RT_LOG_INFO("arena.http", "Account {} ({}) registered", account->Name, account->Id);
    SetJsonResponse(context, http::status::ok, { { "ok", true } });
}

void AuthService::HandleLogin(RequestContext& context)
{
    std::string username, password;
    if (!ReadCredentials(context, username, password))
        return;

    Optional<AccountRow> account = IsValidUsername(username) ? FindAccount(username) : Optional<AccountRow>();
    if (!account || !Rooftree::Crypto::Argon2::Verify(password, account->PasswordHash))
        return SetError(context, http::status::unauthorized, "wrong username or password");

    LoginTicket loginTicket{ .AccountId = account->Id, .Name = account->Name };
    std::string ticket = sTicketMgr->Create(loginTicket, _settings.TicketLifetime);

    RT_LOG_INFO("arena.http", "Account {} ({}) logged in", account->Name, account->Id);
    boost::json::object response
    {
        { "ticket", ticket },
        { "name", account->Name },
        { "wsPort", _settings.WebSocketPort },
        { "wsUrl", _settings.WebSocketUrl },
    };

    // A native client may take UDP instead: the KCP Hello is sealed with this key
    if (_settings.KcpPort)
    {
        response["kcpPort"] = _settings.KcpPort;
        response["kcpKey"] = Rooftree::ToHex(loginTicket.KcpKey);
    }

    SetJsonResponse(context, http::status::ok, std::move(response));
}

void AuthService::HandleGetLeaderboard(RequestContext& context)
{
    ArenaDatabasePreparedStatement* stmt = ArenaDatabase.GetPreparedStatement(ARENA_SEL_LEADERBOARD);
    stmt->setUInt32(0, LeaderboardSize);

    boost::json::array players;
    if (PreparedQueryResult result = ArenaDatabase.Query(stmt))
    {
        do
        {
            Field* fields = result->Fetch();
            players.push_back(boost::json::object{ { "name", fields[0].GetString() }, { "coins", fields[1].GetUInt32() } });
        } while (result->NextRow());
    }

    SetJsonResponse(context, http::status::ok, { { "players", std::move(players) } });
}

Rooftree::Net::Http::RequestHandlerResult ArenaHttpSession::RequestHandler(Rooftree::Net::Http::RequestContext& context)
{
    return sAuthService.HandleRequest(std::static_pointer_cast<ArenaHttpSession>(shared_from_this()), context);
}

