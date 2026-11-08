/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "BlogSession.h"
#include "ApiJson.h"
#include "ApiOperations.h"
#include "BlogDatabase.h"
#include "BlogWorld.h"
#include "Field.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"

namespace http = boost::beast::http;
using Rooftree::Server::Sha256Hex;

namespace Blog
{
namespace
{
// GenerateToken(32): 43 characters of base64url
constexpr std::size_t SessionTokenLength = 43;

void SetSessionCookie(BlogSession::Request const& request, std::string_view token, Seconds maxAge)
{
    BlogSettings const& settings = sWorld->GetSettings();
    Rooftree::Net::Http::SetCookie(request->GetResponse(), {
        .Name = settings.CookieName,
        .Value = std::string(token),
        .MaxAge = maxAge,
        .Secure = settings.CookieSecure,
    });
}

void RespondBanned(BlogSession::Request const& request, UserRow const& user)
{
    request->RespondJson(http::status::forbidden, ErrorToJson(BannedError(user)));
}

// Extends a session that wasn't seen for SessionRenewAfter, so active readers stay logged in
void RenewIfDue(BlogSession::Request const& request, AuthInfo const& auth, std::string_view token)
{
    BlogSettings const& settings = sWorld->GetSettings();
    SystemTimePoint now = SystemTimePoint::clock::now();
    if (now - auth.LastSeenAt < settings.SessionRenewAfter)
        return;

    SystemTimePoint expiresAt = now + settings.SessionLifetime;
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_SESSION_SEEN);
    stmt->setDate(0, expiresAt);
    stmt->setString(1, auth.SessionHash);
    BlogDatabase.Execute(stmt);

    AuthInfo renewed = auth;
    renewed.LastSeenAt = now;
    renewed.ExpiresAt = expiresAt;
    sWorld->CacheAuth(std::move(renewed));
    SetSessionCookie(request, token, settings.SessionLifetime);
}
}

void RespondValidation(BlogSession::Request const& request, boost::json::object fields)
{
    request->RespondError(http::status::bad_request, "validation_failed", {}, std::move(fields));
}

void RespondNotFound(BlogSession::Request const& request)
{
    request->RespondError(http::status::not_found, "not_found");
}

void RespondInternalError(BlogSession::Request const& request)
{
    request->RespondError(http::status::internal_server_error, "internal_error");
}

void BlogSession::RunOperation(Request const& request, ApiOperation const& operation)
{
    AuthHandler run = [this, request, &operation](AuthInfo const* auth)
    {
        JsonRequest json = RequestFromJson(operation, request->GetBody(), request->GetHeader(http::field::content_type), request->GetTarget(),
            request->GetPathParameters());
        if (!json.Code.empty())
            return request->RespondError(json.Status, json.Code);

        Spawn(operation.Op(*this, std::make_shared<ApiCall>(ApiCall::Params{ .Auth = auth ? Optional<AuthInfo>(*auth) : Optional<AuthInfo>(),
            .Opcode = operation.Opcode.Value, .Request = std::move(json.Request), .Errors = std::move(json.Errors),
            .ClientAddress = request->GetClientAddress(), .UserAgent = std::string(request->GetHeader(http::field::user_agent)) },
            [request, &operation](http::status status, std::string payload, LoginChange const& login)
        {
            // Logged out, or a new session for the same account (a password change)
            if (login.Ended)
                SetSessionCookie(request, "", 0s);
            else if (login.NewExpiresAt)
                SetSessionCookie(request, login.NewToken, std::chrono::duration_cast<Seconds>(*login.NewExpiresAt - SystemTimePoint::clock::now()));

            uint32 retryAfter = 0;
            boost::json::value json = AnswerToJson(operation, status, payload, &retryAfter);
            if (retryAfter)
                request->GetResponse().set(http::field::retry_after, std::to_string(retryAfter));
            // A moved one's new place
            if (unsigned(status) >= 300 && unsigned(status) < 400)
                if (boost::json::value const* location = json.at("error").as_object().if_contains("location"))
                    request->GetResponse().set(http::field::location, location->as_string());
            request->RespondJson(status, std::move(json));
        })));
    };

    switch (operation.Access)
    {
        case ApiAccess::Anyone:
            if (operation.CheckOrigin && !Rooftree::Server::IsSafeMethod(request->GetMethod()) && !CheckOrigin(request))
                return;
            return Authenticate(request, std::move(run));
        case ApiAccess::User:
            return RequireUser(request, std::move(run));
        case ApiAccess::UserEvenBanned:
            return RequireUser(request, std::move(run), true);
        case ApiAccess::Moderator:
            return RequireModerator(request, std::move(run));
        case ApiAccess::Admin:
            return RequireAdmin(request, std::move(run));
    }
}

void BlogSession::Authenticate(Request const& request, AuthHandler next)
{
    Optional<std::string_view> cookie = request->GetCookie(sWorld->GetSettings().CookieName);
    if (!cookie || cookie->size() != SessionTokenLength)
        return next(nullptr);

    std::string token(*cookie);
    std::string hash = Sha256Hex(token);
    FindLogin(hash, [request, token = std::move(token), hash, next = std::move(next)](AuthInfo const* auth)
    {
        if (!auth)
        {
            // Expired or logged out elsewhere: drop the cookie too
            SetSessionCookie(request, "", 0s);
            return next(nullptr);
        }

        RenewIfDue(request, *auth, token);
        next(sWorld->FindAuth(hash));
    });
}

void BlogSession::FindLogin(std::string const& sessionHash, AuthHandler next)
{
    if (AuthInfo const* auth = sWorld->FindAuth(sessionHash))
        return next(auth);

    if (sWorld->IsKnownInvalidSession(sessionHash))
        return next(nullptr);

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_SESSION);
    stmt->setString(0, sessionHash);
    AddQueryCallback(BlogDatabase.AsyncQuery(stmt).WithPreparedCallback([hash = sessionHash, next = std::move(next)](PreparedQueryResult result)
    {
        if (!result)
        {
            sWorld->CacheInvalidSession(hash);
            return next(nullptr);
        }

        Field* fields = result->Fetch();
        AuthInfo info;
        info.SessionHash = hash;
        info.CsrfToken = fields[0].GetString();
        info.ExpiresAt = fields[1].GetDate();
        info.LastSeenAt = fields[2].GetDate();
        info.User = UserRow::FromFields(fields + 3);
        sWorld->CacheAuth(std::move(info));
        next(sWorld->FindAuth(hash));
    }));
}

void BlogSession::RequireUser(Request const& request, AuthHandler next, bool allowBanned)
{
    Authenticate(request, [request, next = std::move(next), allowBanned](AuthInfo const* auth)
    {
        if (!auth)
            return request->RespondError(http::status::unauthorized, "unauthenticated");

        if (!Rooftree::Server::IsSafeMethod(request->GetMethod())
            && (!Rooftree::Server::CheckOrigin(request->GetRequest(), sWorld->GetSettings().AllowedOrigins)
                || !Rooftree::Server::CheckCsrfToken(request->GetRequest(), auth->CsrfToken)))
        {
            RT_LOG_INFO("blog.auth", "{} {}: CSRF check failed for {}", request->GetClientAddress().to_string(), request->GetTarget(), auth->User.Username);
            return request->RespondError(http::status::forbidden, "csrf_failed");
        }

        if (!allowBanned && auth->User.IsBanned())
            return RespondBanned(request, auth->User);

        next(auth);
    });
}

Rooftree::Task<Optional<AuthInfo>> BlogSession::RequireUserAsync(Request request, bool allowBanned)
{
    Optional<std::tuple<AuthInfo>> login = co_await Rooftree::WhenCalled<AuthInfo>([&](auto done)
    {
        RequireUser(request, [done](AuthInfo const* auth) { done(*auth); }, allowBanned);
    });
    if (!login)
        co_return {};
    co_return std::get<0>(std::move(*login));
}

void BlogSession::RequireAdmin(Request const& request, AuthHandler next)
{
    RequireUser(request, [request, next = std::move(next)](AuthInfo const* auth)
    {
        if (!auth->User.IsAdmin())
            return request->RespondError(http::status::forbidden, "forbidden");

        next(auth);
    });
}

void BlogSession::RequireModerator(Request const& request, AuthHandler next)
{
    RequireUser(request, [request, next = std::move(next)](AuthInfo const* auth)
    {
        if (!auth->User.CanModerate())
            return request->RespondError(http::status::forbidden, "forbidden");

        next(auth);
    });
}

bool BlogSession::CheckOrigin(Request const& request)
{
    if (Rooftree::Server::CheckOrigin(request->GetRequest(), sWorld->GetSettings().AllowedOrigins))
        return true;

    RT_LOG_INFO("blog.auth", "{} {}: refused, Origin {} is not ours", request->GetClientAddress().to_string(), request->GetTarget(),
        Rooftree::Server::GetRequestOrigin(request->GetRequest()));
    request->RespondError(http::status::forbidden, "origin_denied");
    return false;
}

std::string BlogSession::StartAuthSession(Request const& request, UserRow const& user, std::string const& token, std::string const& sessionHash,
    std::string const& csrfToken, SystemTimePoint expiresAt)
{
    AuthInfo info;
    info.SessionHash = sessionHash;
    info.CsrfToken = csrfToken;
    info.ExpiresAt = expiresAt;
    info.LastSeenAt = SystemTimePoint::clock::now();
    info.User = user;
    sWorld->CacheAuth(std::move(info));

    SetSessionCookie(request, token, std::chrono::duration_cast<Seconds>(expiresAt - SystemTimePoint::clock::now()));
    return csrfToken;
}
}
