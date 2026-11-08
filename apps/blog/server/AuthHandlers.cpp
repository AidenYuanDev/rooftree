/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Accounts: register (with a mailed verification link), log in (TOTP for whoever turned it on), log out, the current user, password reset by mail, password change, two-step login setup.
 *
 * GitHub and Google logins (OAuth.h for the provider side): POST /api/auth/oauth/<provider> gives the
 * address to send the browser to and a cookie with the state; the provider sends it back to the
 * callback, which checks the state against the cookie, reads the provider's user on a worker and
 *  - logs in the account linked to it,
 *  - or makes a new account with the provider's verified address (no password, no captcha, no mail),
 *    unless an account has that address: then its owner logs in with the password and links the
 *    provider on the account page, an address alone never merges accounts,
 *  - or, when linking, links it to the logged in account.
 * Accounts with two-step login get a ticket cookie and type the code on the login page.
 *
 * The pattern of every handler: validate the request and check the limits on the main thread, run the
 * dependent queries and Argon2 on a worker (the Worker* functions, which only see their arguments),
 * then update the auth cache and respond on the main thread again.
 *
 * Nothing tells an anonymous client whether an account exists: a wrong user name and a wrong password
 * give the same error after the same Argon2 work, "forgot password" answers ok for any address.
 */

#include "Accounts.h"
#include "ApiCall.h"
#include "ApiJson.h"
#include "Argon2Hash.h"
#include "Avatar.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "CryptoRandom.h"
#include "Field.h"
#include "FlatJson.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "OAuth.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "Random.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include "TOTP.h"
#include "Util.h"
#include "Validation.h"
#include <mutex>

namespace http = boost::beast::http;
using Rooftree::Server::GenerateToken;
using Rooftree::Server::Sha256Hex;

namespace Blog
{
namespace
{
constexpr std::size_t TotpSecretSize = Rooftree::Crypto::TOTP::SecretSize;
}

HashSettings GetHashSettings()
{
    BlogSettings const& settings = sWorld->GetSettings();
    return { settings.Argon2Iterations, settings.Argon2MemoryCost };
}

Optional<std::string> HashPassword(std::string const& password, HashSettings settings)
{
    return Rooftree::Crypto::Argon2::Hash(password, settings.Iterations, settings.MemoryCost);
}

namespace
{
// Verified against when the account doesn't exist, so that takes as long as a wrong password
std::string const& GetDummyHash(HashSettings settings)
{
    static std::once_flag once;
    static std::string hash;
    std::call_once(once, [&] { hash = HashPassword(GenerateToken(), settings).value_or(""); });
    return hash;
}

std::string ToLower(std::string_view text)
{
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), Rooftree::CharToLower);
    return lower;
}

// The time step of a valid code that wasn't used before, within one step of now
Optional<int64> VerifyTotp(std::vector<uint8> const& secret, std::string_view code, Optional<int64> lastStep)
{
    if (code.size() != 6 || !std::ranges::all_of(code, [](char c) { return c >= '0' && c <= '9'; }))
        return {};

    uint32 value = uint32(std::stoul(std::string(code)));
    int64 now = int64(std::time(nullptr)) / 30;
    for (int64 step = now - 1; step <= now + 1; ++step)
        if ((!lastStep || step > *lastStep) && Rooftree::Crypto::TOTP::GenerateToken(secret, time_t(step * 30)) == value)
            return step;

    return {};
}

std::string MailLink(std::string_view locale, std::string_view page, std::string_view token)
{
    return Rooftree::StringFormat("{}/{}/{}?token={}", sWorld->GetSettings().PublicOrigin, locale, page, token);
}
}

Rooftree::Server::MailMessage VerificationMail(UserRow const& user, std::string_view token)
{
    std::string site = sWorld->GetSiteName(user.Locale);
    std::string link = MailLink(user.Locale, "verify-email", token);
    if (user.Locale == "en")
        return { .To = user.Email, .Subject = "Confirm your email address for " + site,
            .TextBody = Rooftree::StringFormat("Hi {},\n\nopen this link to confirm your email address:\n\n{}\n\nIt works for 24 hours. "
                "If you didn't register at {}, ignore this mail.\n", user.DisplayName, link, site) };

    return { .To = user.Email, .Subject = "确认你在" + site + "的邮箱",
        .TextBody = Rooftree::StringFormat("{}，你好：\n\n打开下面的链接确认你的邮箱：\n\n{}\n\n链接 24 小时内有效。如果你没有在{}注册，请忽略这封邮件。\n",
            user.DisplayName, link, site) };
}

Rooftree::Server::MailMessage ResetMail(UserRow const& user, std::string_view token)
{
    std::string site = sWorld->GetSiteName(user.Locale);
    std::string link = MailLink(user.Locale, "reset-password", token);
    if (user.Locale == "en")
        return { .To = user.Email, .Subject = "Reset your password for " + site,
            .TextBody = Rooftree::StringFormat("Hi {},\n\nopen this link to choose a new password:\n\n{}\n\nIt works for one hour. "
                "If you didn't ask for it, ignore this mail, your password stays as it is.\n", user.DisplayName, link) };

    return { .To = user.Email, .Subject = "重置你在" + site + "的密码",
        .TextBody = Rooftree::StringFormat("{}，你好：\n\n打开下面的链接设置新密码：\n\n{}\n\n链接 1 小时内有效。如果不是你本人操作，请忽略这封邮件，密码不会改变。\n",
            user.DisplayName, link) };
}

Rooftree::Server::MailMessage InviteMail(UserRow const& user, std::string_view token, Seconds lifetime)
{
    std::string site = sWorld->GetSiteName(user.Locale);
    std::string link = MailLink(user.Locale, "reset-password", token);
    int64 hours = std::chrono::duration_cast<std::chrono::hours>(lifetime).count();
    if (user.Locale == "en")
        return { .To = user.Email, .Subject = "Your account at " + site,
            .TextBody = Rooftree::StringFormat("Hi {},\n\nan account was made for you at {}, your username is {}. Open this link to choose its password:\n\n{}\n\n"
                "It works for {} hours; afterwards, \"Forgot password\" on the login page mails a new one.\n", user.DisplayName, site, user.Username, link, hours) };

    return { .To = user.Email, .Subject = "你在" + site + "的账号",
        .TextBody = Rooftree::StringFormat("{}，你好：\n\n管理员为你在{}创建了账号，用户名是 {}。打开下面的链接设置密码：\n\n{}\n\n"
            "链接 {} 小时内有效；过期以后可以在登录页点「忘记密码」重新获取。\n", user.DisplayName, site, user.Username, link, hours) };
}

namespace
{
Optional<UserRow> FindUser(BlogDatabaseStatements statement, std::string const& key)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(statement);
    stmt->setString(0, key);
    if (statement == BLOG_SEL_USER_FOR_LOGIN)
        stmt->setString(1, key);

    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    return UserRow::FromFields(result->Fetch());
}
}

Optional<UserRow> FindUserById(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_BY_ID);
    stmt->setUInt32(0, id);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    return UserRow::FromFields(result->Fetch());
}

std::string WorkerCreateEmailToken(uint32 userId, std::string_view purpose, Seconds lifetime)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_EMAIL_TOKENS);
    stmt->setUInt32(0, userId);
    stmt->setString(1, purpose);
    BlogDatabase.DirectExecute(stmt);

    std::string token = GenerateToken();
    stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_EMAIL_TOKEN);
    stmt->setString(0, Sha256Hex(token));
    stmt->setUInt32(1, userId);
    stmt->setString(2, purpose);
    stmt->setDate(3, SystemTimePoint::clock::now() + lifetime);
    BlogDatabase.DirectExecute(stmt);
    return token;
}

namespace
{
// Worker: the user of an unused, unexpired token, which is used up then
Optional<uint32> WorkerUseEmailToken(std::string const& token, std::string_view purpose)
{
    std::string hash = Sha256Hex(token);
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_EMAIL_TOKEN);
    stmt->setString(0, hash);
    stmt->setString(1, purpose);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    uint32 userId = result->Fetch()[0].GetUInt32();
    stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_EMAIL_TOKEN_USED);
    stmt->setString(0, hash);
    BlogDatabase.DirectExecute(stmt);
    return userId;
}
}

NewAuthSession WorkerCreateAuthSession(uint32 userId, Seconds lifetime, std::string const& ip, std::string const& userAgent)
{
    NewAuthSession session{ .Token = GenerateToken(), .CsrfToken = GenerateToken(), .ExpiresAt = SystemTimePoint::clock::now() + lifetime };
    session.Hash = Sha256Hex(session.Token);

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_SESSION);
    stmt->setString(0, session.Hash);
    stmt->setUInt32(1, userId);
    stmt->setString(2, session.CsrfToken);
    stmt->setDate(3, session.ExpiresAt);
    stmt->setString(4, ip);
    stmt->setString(5, userAgent.substr(0, 255));
    BlogDatabase.DirectExecute(stmt);
    return session;
}

void WorkerDeleteUserSessions(uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_SESSIONS);
    stmt->setUInt32(0, userId);
    BlogDatabase.DirectExecute(stmt);
}

bool WorkerSetPassword(uint32 userId, std::string const& password, HashSettings hashSettings)
{
    Optional<std::string> hash = HashPassword(password, hashSettings);
    if (!hash)
        return false;

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_PASSWORD);
    stmt->setString(0, *hash);
    stmt->setUInt32(1, userId);
    BlogDatabase.DirectExecute(stmt);
    return true;
}

namespace
{
std::string GetUserAgent(BlogSession::Request const& request)
{
    return std::string(request->GetHeader(http::field::user_agent));
}

// The account as its own pages see it
flatbuffers::Offset<Reader::Account> AccountTable(flatbuffers::FlatBufferBuilder& builder, UserRow const& user)
{
    return Reader::CreateAccount(builder, user.Id, builder.CreateString(user.Username), builder.CreateString(user.DisplayName),
        builder.CreateString(user.Role), user.Avatar.empty() ? 0 : builder.CreateString("/uploads/" + user.Avatar), builder.CreateString(user.Email),
        user.EmailVerified, builder.CreateString(user.Locale), user.TotpSecret.has_value(), user.HasPassword,
        user.IsBanned() ? TimeString(builder, *user.BannedUntil) : 0, StringList(builder, user.Denied), user.IsGranted(GrantDrive), user.ReplyMails,
        user.IsGranted(GrantWrite), user.EditsPosts);
}

// The routes that log in answer with the account and the token too: a Me, as the Me operation does
boost::json::object MeJson(UserRow const& user, std::string_view csrfToken)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateMe(builder, AccountTable(builder, user), builder.CreateString(csrfToken)));
    return GetApiSchema().ToJson(*GetApiSchema().Find("Blog.Reader.Me"), { builder.GetBufferPointer(), builder.GetSize() });
}

void RespondProfile(ApiRequest const& request, UserRow const& user)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateProfile(builder, AccountTable(builder, user)));
    request->Respond(builder);
}

// "required" or "wrong" for the picture's id and what was typed, none when right
std::string_view CaptchaError(std::string const& id, std::string const& answer)
{
    if (id.empty() || answer.empty())
        return "required";
    if (!sWorld->GetCaptchas().Check(id, answer))
        return "wrong";
    return {};
}
}

Rooftree::Task<void> GetCaptcha(Rooftree::Server::Session& /*session*/, ApiRequest request)
{
    flatbuffers::FlatBufferBuilder builder;
    if (!sWorld->GetSettings().CaptchaEnabled)
        builder.Finish(Reader::CreateCaptcha(builder, false));
    else
    {
        Rooftree::Server::CaptchaStore::Issued captcha = sWorld->GetCaptchas().Issue();
        builder.Finish(Reader::CreateCaptchaDirect(builder, true, captcha.Id.c_str(),
            ("data:image/png;base64," + Rooftree::Server::Base64Encode(captcha.Image.Png)).c_str(), captcha.Image.Width, captcha.Image.Height));
    }
    request->Respond(builder);
    co_return;
}

bool BlogSession::CheckCaptcha(RequestFields& fields)
{
    if (!sWorld->GetSettings().CaptchaEnabled)
        return true;

    std::string_view error = CaptchaError(fields.Text("captchaId", 0, 64, false), fields.Text("captcha", 0, 16, false));
    if (!error.empty())
        fields.AddError("captcha", error);
    return error.empty();
}

bool CheckCaptcha(TableFields& fields, flatbuffers::String const* id, flatbuffers::String const* answer)
{
    if (!sWorld->GetSettings().CaptchaEnabled)
        return true;

    std::string_view error = CaptchaError(fields.Text("captchaId", id, 0, 64, false), fields.Text("captcha", answer, 0, 16, false));
    if (!error.empty())
        fields.AddError("captcha", error);
    return error.empty();
}

bool CheckMailInterval(ApiRequest const& request, std::string const& email)
{
    std::string key = ToLower(email);
    if (sWorld->GetMailLimiter().Allow(key))
        return true;

    TimePoint now = TimePoint::clock::now();
    request->RespondError({ .Status = uint16(http::status::too_many_requests), .Code = "too_many_mails",
        .RetryAfter = uint32(std::chrono::duration_cast<Seconds>(sWorld->GetMailLimiter().GetRetryTime(key, now) - now).count() + 1) });
    return false;
}

Rooftree::Task<void> BlogSession::HandleRegister(Request request)
{
    if (!CheckOrigin(request))
        co_return;

    // IP bans are for waves of throwaway accounts
    if (sWorld->IsIpBanned(request->GetClientAddress()))
        co_return request->RespondError(http::status::forbidden, "ip_banned");

    Optional<boost::json::object> body = Rooftree::Server::ParseJsonObject(request->GetBody());
    if (!body)
        co_return request->RespondError(http::status::bad_request, "invalid_json");

    RequestFields fields(*body);
    std::string username = fields.Username("username");
    std::string displayName = fields.Text("displayName", 1, 40);
    std::string email = fields.Email("email");
    std::string password = fields.Password("password");
    std::string locale = fields.Lang("locale", false);
    if (!fields.IsValid() || !CheckCaptcha(fields))
        co_return RespondValidation(request, fields.TakeErrors());
    if (locale.empty())
        locale = "zh";

    struct Result
    {
        bool UsernameTaken = false;
        bool EmailTaken = false;
        bool Failed = false;
        UserRow User;
        std::string Token;
    };

    HashSettings hashSettings = GetHashSettings();
    Seconds tokenLifetime = sWorld->GetSettings().EmailVerificationLifetime;
    Result result = co_await OnWorker([=]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CONFLICTS);
        stmt->setString(0, username);
        stmt->setString(1, email);
        stmt->setString(2, username);
        stmt->setString(3, email);
        if (PreparedQueryResult conflicts = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* row = conflicts->Fetch();
                result.UsernameTaken |= row[0].GetInt64() != 0;
                result.EmailTaken |= row[1].GetInt64() != 0;
            } while (conflicts->NextRow());
            return result;
        }

        Optional<std::string> hash = HashPassword(password, hashSettings);
        if (!hash)
        {
            result.Failed = true;
            return result;
        }

        stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER);
        stmt->setString(0, username);
        stmt->setString(1, displayName);
        stmt->setString(2, email);
        stmt->setString(3, *hash);
        stmt->setString(4, RoleUser);
        stmt->setString(5, locale);
        stmt->setNull(6);
        stmt->setNull(7);
        BlogDatabase.DirectExecute(stmt);

        // Two registrations at once: the unique keys let one INSERT through, the hash tells whose it was
        Optional<UserRow> user = FindUser(BLOG_SEL_USER_FOR_LOGIN, username);
        if (!user || user->PasswordHash != *hash)
        {
            result.UsernameTaken = true;
            return result;
        }

        result.User = std::move(*user);
        result.Token = WorkerCreateEmailToken(result.User.Id, "verify", tokenLifetime);
        return result;
    });
    if (result.Failed)
        co_return RespondInternalError(request);

    if (result.UsernameTaken || result.EmailTaken)
    {
        boost::json::object conflicts;
        if (result.UsernameTaken)
            conflicts["username"] = "taken";
        if (result.EmailTaken)
            conflicts["email"] = "taken";
        co_return request->RespondError(http::status::conflict, "conflict", {}, std::move(conflicts));
    }

    RT_LOG_INFO("blog.auth", "{} registered {} ({})", request->GetClientAddress().to_string(), result.User.Username, result.User.Id);
    sWorld->GetMailLimiter().Allow(ToLower(result.User.Email));
    sWorld->SendMail(*this, VerificationMail(result.User, result.Token));
    request->RespondJson(http::status::created, { { "ok", true } });
}

Rooftree::Task<void> BlogSession::HandleLogin(Request request)
{
    if (!CheckOrigin(request))
        co_return;

    Optional<boost::json::object> body = Rooftree::Server::ParseJsonObject(request->GetBody());
    if (!body)
        co_return request->RespondError(http::status::bad_request, "invalid_json");

    RequestFields fields(*body);
    std::string login = fields.Text("login", 1, 254);
    std::string password = fields.Text("password", 1, 128);
    std::string totp = fields.Text("totp", 0, 6, false);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    // Per login name, whoever tries it from wherever: AntiDOS already limits each address
    std::string failureKey = ToLower(login);
    TimePoint retryAt = sWorld->GetLoginFailures().GetRetryTime(failureKey);
    if (retryAt > TimePoint::clock::now())
    {
        request->GetResponse().set(http::field::retry_after,
            std::to_string(std::chrono::duration_cast<Seconds>(retryAt - TimePoint::clock::now()).count() + 1));
        co_return request->RespondError(http::status::too_many_requests, "too_many_attempts");
    }

    enum class LoginOutcome
    {
        Ok,
        InvalidCredentials,
        TotpRequired,
        TotpInvalid,
        Banned
    };

    struct Result
    {
        LoginOutcome Outcome = LoginOutcome::InvalidCredentials;
        UserRow User;
        NewAuthSession Session;
    };

    HashSettings hashSettings = GetHashSettings();
    Seconds lifetime = sWorld->GetSettings().SessionLifetime;
    std::string ip = request->GetClientAddress().to_string();
    std::string userAgent = GetUserAgent(request);
    Result result = co_await OnWorker([=]
    {
        Result result;
        Optional<UserRow> user = FindUser(BLOG_SEL_USER_FOR_LOGIN, login);
        if (!user)
        {
            Rooftree::Crypto::Argon2::Verify(password, GetDummyHash(hashSettings));
            return result;
        }

        if (!Rooftree::Crypto::Argon2::Verify(password, user->PasswordHash))
            return result;

        // Only someone with the password learns about the second step or the ban
        if (user->TotpSecret)
        {
            if (totp.empty())
            {
                result.Outcome = LoginOutcome::TotpRequired;
                return result;
            }

            Optional<int64> step = VerifyTotp(*user->TotpSecret, totp, user->TotpLastStep);
            if (!step)
            {
                result.Outcome = LoginOutcome::TotpInvalid;
                return result;
            }

            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_STEP);
            stmt->setInt64(0, *step);
            stmt->setUInt32(1, user->Id);
            BlogDatabase.DirectExecute(stmt);
            user->TotpLastStep = step;
        }

        if (user->IsBanned())
        {
            result.Outcome = LoginOutcome::Banned;
            result.User = std::move(*user);
            return result;
        }

        result.Session = WorkerCreateAuthSession(user->Id, lifetime, ip, userAgent);
        result.User = std::move(*user);
        result.Outcome = LoginOutcome::Ok;
        return result;
    });
    switch (result.Outcome)
    {
        case LoginOutcome::InvalidCredentials:
        case LoginOutcome::TotpInvalid:
            sWorld->GetLoginFailures().Allow(failureKey);
            RT_LOG_INFO("blog.auth", "{} failed to log in as {}", request->GetClientAddress().to_string(), failureKey);
            co_return request->RespondError(http::status::unauthorized,
                result.Outcome == LoginOutcome::TotpInvalid ? "totp_invalid" : "invalid_credentials");
        case LoginOutcome::TotpRequired:
            co_return request->RespondError(http::status::unauthorized, "totp_required");
        case LoginOutcome::Banned:
        {
            boost::json::object error{ { "code", "banned" }, { "until", FormatTime(*result.User.BannedUntil) } };
            if (!result.User.BanReason.empty())
                error["reason"] = result.User.BanReason;
            co_return request->RespondJson(http::status::forbidden, { { "error", std::move(error) } });
        }
        case LoginOutcome::Ok:
            break;
    }

    RT_LOG_INFO("blog.auth", "{} logged in as {} ({})", request->GetClientAddress().to_string(), result.User.Username, result.User.Id);
    BlogSession& blogSession = *this;
    std::string csrf = blogSession.StartAuthSession(request, result.User, result.Session.Token, result.Session.Hash,
        result.Session.CsrfToken, result.Session.ExpiresAt);
    request->RespondJson(http::status::ok, MeJson(result.User, csrf));
}

Rooftree::Task<void> Logout(Rooftree::Server::Session& /*session*/, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_SESSION);
    stmt->setString(0, auth.SessionHash);
    BlogDatabase.Execute(stmt);

    RT_LOG_INFO("blog.auth", "{} logged out", auth.User.Username);
    sWorld->ForgetAuth(auth.SessionHash);
    // A route clears the cookie, a connection calls as nobody from now on
    request->EndLogin();
    RespondOk(request);
    co_return;
}

Rooftree::Task<void> GetMe(Rooftree::Server::Session& /*session*/, ApiRequest request)
{
    flatbuffers::FlatBufferBuilder builder;
    if (AuthInfo const* auth = request->GetLogin())
        builder.Finish(Reader::CreateMe(builder, AccountTable(builder, auth->User), builder.CreateString(auth->CsrfToken)));
    else
        builder.Finish(Reader::CreateMe(builder));
    request->Respond(builder);
    co_return;
}

Rooftree::Task<void> UpdateMe(Rooftree::Server::Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    Reader::MeUpdateRequest const& in = request->Get<Reader::MeUpdateRequest>();
    TableFields fields(request->GetErrors());
    std::string displayName = in.displayName() ? fields.Text("displayName", in.displayName(), 1, 40) : auth.User.DisplayName;
    std::string locale = in.locale() ? fields.Lang("locale", in.locale()) : auth.User.Locale;
    bool replyMails = in.replyMails().value_or(auth.User.ReplyMails);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    uint32 userId = auth.User.Id;
    Optional<UserRow> user = co_await session.OnWorker([=]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_PROFILE);
        stmt->setString(0, displayName);
        stmt->setString(1, locale);
        stmt->setBool(2, replyMails);
        stmt->setUInt32(3, userId);
        BlogDatabase.DirectExecute(stmt);
        return FindUserById(userId);
    });
    if (!user)
        co_return RespondNotFound(request);

    sWorld->UpdateCachedUser(user->Id, [&](UserRow& cached)
    {
        cached.DisplayName = user->DisplayName;
        cached.Locale = user->Locale;
        cached.ReplyMails = user->ReplyMails;
    });
    RespondProfile(request, *user);
}

Rooftree::Task<void> VerifyEmail(Rooftree::Server::Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    std::string token = fields.Text("token", request->Get<Reader::TokenRequest>().token(), 43, 43);
    if (!fields.IsValid())
        co_return request->RespondError(http::status::bad_request, "invalid_token");

    Optional<uint32> userId = co_await session.OnWorker([token]
    {
        Optional<uint32> userId = WorkerUseEmailToken(token, "verify");
        if (userId)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_VERIFIED);
            stmt->setUInt32(0, *userId);
            BlogDatabase.DirectExecute(stmt);
        }
        return userId;
    });
    if (!userId)
        co_return request->RespondError(http::status::bad_request, "invalid_token");

    sWorld->UpdateCachedUser(*userId, [](UserRow& user) { user.EmailVerified = true; });
    RespondOk(request);
}

Rooftree::Task<void> ResendVerification(Rooftree::Server::Session& session, ApiRequest request)
{
    UserRow const& user = request->GetAuth().User;
    if (user.EmailVerified)
        co_return request->RespondError(http::status::conflict, "already_verified");

    if (!CheckMailInterval(request, user.Email))
        co_return;

    Seconds lifetime = sWorld->GetSettings().EmailVerificationLifetime;
    std::string token = co_await session.OnWorker([userId = user.Id, lifetime]
    {
        return WorkerCreateEmailToken(userId, "verify", lifetime);
    });
    sWorld->SendMail(session, VerificationMail(user, token));
    RespondOk(request);
}

Rooftree::Task<void> ForgotPassword(Rooftree::Server::Session& session, ApiRequest request)
{
    Reader::EmailRequest const& in = request->Get<Reader::EmailRequest>();
    TableFields fields(request->GetErrors());
    std::string email = fields.Email("email", in.email());
    if (!fields.IsValid() || !CheckCaptcha(fields, in.captchaId(), in.captcha()))
        co_return RespondValidation(request, fields.TakeErrors());

    // Too soon after the last mail: the same answer, just no mail
    if (!sWorld->GetMailLimiter().Allow(ToLower(email)))
    {
        RT_LOG_INFO("blog.auth", "{}: too many mails to {}, not sending a reset link", request->GetClientAddress().to_string(), email);
        co_return RespondOk(request);
    }

    struct Result
    {
        Optional<UserRow> User;
        std::string Token;
    };

    Seconds lifetime = sWorld->GetSettings().PasswordResetLifetime;
    Result result = co_await session.OnWorker([email, lifetime]
    {
        Result result;
        result.User = FindUser(BLOG_SEL_USER_BY_EMAIL, email);
        if (result.User)
            result.Token = WorkerCreateEmailToken(result.User->Id, "reset", lifetime);
        return result;
    });
    if (result.User)
    {
        RT_LOG_INFO("blog.auth", "{} asked for a password reset of {}", request->GetClientAddress().to_string(), result.User->Username);
        sWorld->SendMail(session, ResetMail(*result.User, result.Token));
    }

    RespondOk(request);
}

Rooftree::Task<void> ResetPassword(Rooftree::Server::Session& session, ApiRequest request)
{
    Reader::PasswordResetRequest const& in = request->Get<Reader::PasswordResetRequest>();
    TableFields fields(request->GetErrors());
    std::string token = fields.Text("token", in.token(), 43, 43);
    std::string password = fields.Password("password", in.password());
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    HashSettings hashSettings = GetHashSettings();
    Optional<uint32> userId = co_await session.OnWorker([=]() -> Optional<uint32>
    {
        Optional<uint32> userId = WorkerUseEmailToken(token, "reset");
        if (!userId || !WorkerSetPassword(*userId, password, hashSettings))
            return {};

        WorkerDeleteUserSessions(*userId);
        // Following the link proved the address is theirs
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_VERIFIED);
        stmt->setUInt32(0, *userId);
        BlogDatabase.DirectExecute(stmt);
        return userId;
    });
    if (!userId)
        co_return request->RespondError(http::status::bad_request, "invalid_token");

    RT_LOG_INFO("blog.auth", "{} reset the password of user {}", request->GetClientAddress().to_string(), *userId);
    sWorld->ForgetUserAuth(*userId);
    RespondOk(request);
}

Rooftree::Task<void> ChangePassword(Rooftree::Server::Session& session, ApiRequest request)
{
    AuthInfo const& auth = request->GetAuth();
    Reader::PasswordChangeRequest const& in = request->Get<Reader::PasswordChangeRequest>();
    TableFields fields(request->GetErrors());
    // An account made by a GitHub or Google login sets its first one without
    std::string currentPassword = fields.Text("currentPassword", in.currentPassword(), 1, 128, auth.User.HasPassword);
    std::string newPassword = fields.Password("newPassword", in.newPassword());
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        bool Ok = false;
        Optional<UserRow> User;
        NewAuthSession Session;
    };

    uint32 userId = auth.User.Id;
    HashSettings hashSettings = GetHashSettings();
    Seconds lifetime = sWorld->GetSettings().SessionLifetime;
    std::string ip = request->GetClientAddress().to_string();
    std::string userAgent = request->GetUserAgent();
    Result result = co_await session.OnWorker([=]
    {
        Result result;
        result.User = FindUserById(userId);
        if (!result.User || (result.User->HasPassword && !Rooftree::Crypto::Argon2::Verify(currentPassword, result.User->PasswordHash))
            || !WorkerSetPassword(userId, newPassword, hashSettings))
            return result;
        result.User->HasPassword = true;

        // Every session ends, this one gets a new one
        WorkerDeleteUserSessions(userId);
        result.Session = WorkerCreateAuthSession(userId, lifetime, ip, userAgent);
        result.Ok = true;
        return result;
    });
    if (!result.Ok)
        co_return RespondValidation(request, { { "currentPassword", "incorrect" } });

    RT_LOG_INFO("blog.auth", "{} changed their password", result.User->Username);
    sWorld->ForgetUserAuth(result.User->Id);
    AuthInfo info;
    info.SessionHash = result.Session.Hash;
    info.CsrfToken = result.Session.CsrfToken;
    info.ExpiresAt = result.Session.ExpiresAt;
    info.LastSeenAt = SystemTimePoint::clock::now();
    info.User = *result.User;
    sWorld->CacheAuth(std::move(info));
    // A route sets the new session's cookie, a connection calls as it from now on
    request->StartLogin(result.Session.Token, result.Session.Hash, result.Session.ExpiresAt);

    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateMe(builder, AccountTable(builder, *result.User), builder.CreateString(result.Session.CsrfToken)));
    request->Respond(builder);
}

Rooftree::Task<void> SetupTotp(Rooftree::Server::Session& session, ApiRequest request)
{
    UserRow const& user = request->GetAuth().User;
    if (user.TotpSecret)
        co_return request->RespondError(http::status::conflict, "totp_already_enabled");

    std::vector<uint8> secret(TotpSecretSize);
    Rooftree::Crypto::GetRandomBytes(secret);
    auto account = user.Username;
    co_await session.OnWorker([userId = user.Id, secret]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_PENDING);
        stmt->setBinary(0, std::span<uint8 const>(secret));
        stmt->setUInt32(1, userId);
        BlogDatabase.DirectExecute(stmt);
    });
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateTotpSecretDirect(builder, Rooftree::Server::Base32Encode(secret).c_str(),
        Rooftree::Server::BuildTotpUri(sWorld->GetSettings().TotpIssuer, account, secret).c_str()));
    request->Respond(builder);
}

Rooftree::Task<void> EnableTotp(Rooftree::Server::Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    std::string code = fields.Text("code", request->Get<Reader::TotpCodeRequest>().code(), 6, 6);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    uint32 userId = request->GetAuth().User.Id;
    Optional<std::vector<uint8>> secret = co_await session.OnWorker([userId, code]() -> Optional<std::vector<uint8>>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_TOTP_PENDING);
        stmt->setUInt32(0, userId);
        PreparedQueryResult result = BlogDatabase.Query(stmt);
        Optional<std::vector<uint8>> secret = result ? result->Fetch()[0].GetBinaryOrNull() : Optional<std::vector<uint8>>();
        if (!secret)
            return {};

        Optional<int64> step = VerifyTotp(*secret, code, {});
        if (!step)
            return {};

        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_ENABLE);
        stmt->setInt64(0, *step);
        stmt->setUInt32(1, userId);
        BlogDatabase.DirectExecute(stmt);
        return secret;
    });
    if (!secret)
        co_return RespondValidation(request, { { "code", "incorrect" } });

    sWorld->UpdateCachedUser(userId, [&](UserRow& user) { user.TotpSecret = secret; });
    RespondOk(request);
}

Rooftree::Task<void> DisableTotp(Rooftree::Server::Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    std::string password = fields.Text("password", request->Get<Reader::PasswordRequest>().password(), 1, 128);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    uint32 userId = request->GetAuth().User.Id;
    bool ok = co_await session.OnWorker([userId, password]
    {
        Optional<UserRow> user = FindUserById(userId);
        if (!user || !Rooftree::Crypto::Argon2::Verify(password, user->PasswordHash))
            return false;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_DISABLE);
        stmt->setUInt32(0, userId);
        BlogDatabase.DirectExecute(stmt);
        return true;
    });
    if (!ok)
        co_return RespondValidation(request, { { "password", "incorrect" } });

    sWorld->UpdateCachedUser(userId, [](UserRow& user) { user.TotpSecret.reset(); });
    RespondOk(request);
}

namespace
{
constexpr std::string_view OAuthStateCookie = "blog_oauth";
constexpr std::string_view OAuthTicketCookie = "blog_oauth_ticket";
constexpr Seconds OAuthFlowLifetime = 10min;
constexpr Seconds OAuthTicketLifetime = 5min;
constexpr uint32 OAuthTicketAttempts = 5;

std::string OAuthRedirectUri(std::string_view provider)
{
    return Rooftree::StringFormat("{}/api/auth/oauth/{}/callback", sWorld->GetSettings().PublicOrigin, provider);
}

// A page of ours to go back to: a path, never another site's address
std::string SafeNext(std::string_view next, std::string_view fallback)
{
    if (next.size() > 200 || !next.starts_with('/') || next.starts_with("//") || next.find_first_of("\\\r\n") != std::string_view::npos)
        return std::string(fallback);
    return std::string(next);
}

void SetOAuthCookie(BlogSession::Request const& request, std::string_view name, std::string_view value, Seconds maxAge)
{
    Rooftree::Net::Http::SetCookie(request->GetResponse(), { .Name = std::string(name), .Value = std::string(value), .Path = "/api/auth",
        .MaxAge = maxAge, .Secure = sWorld->GetSettings().CookieSecure });
}

void RespondRedirect(BlogSession::Request const& request, std::string const& location)
{
    request->GetResponse().result(http::status::found);
    request->GetResponse().set(http::field::location, location);
    request->GetResponse().set(http::field::cache_control, "no-store");
    request->Respond();
}

// Where the browser goes when a GitHub or Google login didn't work: the login page, or the account
// page when linking, with the reason
void RedirectOAuthError(BlogSession::Request const& request, BlogWorld::OAuthFlow const& flow, std::string_view code)
{
    RespondRedirect(request, Rooftree::StringFormat("{}/{}/{}?oauth_error={}&provider={}", sWorld->GetSettings().PublicOrigin, flow.Lang,
        flow.LinkUserId ? "account" : "login", code, flow.Provider));
}

// Worker: a new account for a provider's user, its name made unique. Empty when the address turned
// out to be someone's meanwhile
Optional<UserRow> WorkerCreateOAuthUser(OAuthIdentity const& identity, std::string const& lang)
{
    std::string base = SuggestUsername(identity);
    std::string displayName = identity.Name.empty() ? identity.Login : identity.Name;
    // At most 40 characters, not bytes: cut before the 41st character's first byte
    for (std::size_t i = 0, characters = 0; i < displayName.size(); ++i)
    {
        if ((uint8(displayName[i]) & 0xC0) != 0x80 && ++characters > 40)
        {
            displayName.resize(i);
            break;
        }
    }
    if (displayName.empty())
        displayName = base;

    for (int32 attempt = 0; attempt < 8; ++attempt)
    {
        std::string username = attempt == 0 ? base : base.substr(0, 16) + std::to_string(urand(1000, 9999));
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER);
        stmt->setString(0, username);
        stmt->setString(1, displayName);
        stmt->setString(2, identity.Email);
        stmt->setNull(3);
        stmt->setString(4, RoleUser);
        stmt->setString(5, lang);
        stmt->setDate(6, SystemTimePoint::clock::now());
        stmt->setNull(7);
        BlogDatabase.DirectExecute(stmt);

        Optional<UserRow> user = FindUser(BLOG_SEL_USER_BY_EMAIL, identity.Email);
        if (user)
            return ToLower(user->Username) == ToLower(username) && !user->HasPassword ? user : Optional<UserRow>();
    }
    return {};
}

bool WorkerInsertIdentity(uint32 userId, std::string_view provider, OAuthIdentity const& identity)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_IDENTITY);
    stmt->setUInt32(0, userId);
    stmt->setString(1, provider);
    stmt->setString(2, identity.ProviderUserId);
    stmt->setString(3, identity.Login.substr(0, 254));
    if (identity.AvatarUrl.empty())
        stmt->setNull(4);
    else
        stmt->setString(4, identity.AvatarUrl);
    BlogDatabase.DirectExecute(stmt);

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_BY_IDENTITY);
    stmt->setString(0, provider);
    stmt->setString(1, identity.ProviderUserId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    return result && result->Fetch()[0].GetUInt32() == userId;
}

// The login and picture as the provider has them now
void WorkerRefreshIdentity(std::string_view provider, OAuthIdentity const& identity)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_IDENTITY_PROFILE);
    stmt->setString(0, identity.Login.substr(0, 254));
    if (identity.AvatarUrl.empty())
        stmt->setNull(1);
    else
        stmt->setString(1, identity.AvatarUrl);
    stmt->setString(2, provider);
    stmt->setString(3, identity.ProviderUserId);
    BlogDatabase.DirectExecute(stmt);
}

Optional<UserRow> WorkerFindUserByIdentity(std::string_view provider, std::string const& providerUserId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_BY_IDENTITY);
    stmt->setString(0, provider);
    stmt->setString(1, providerUserId);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};
    return UserRow::FromFields(result->Fetch());
}
}

Rooftree::Task<void> ListOAuthProviders(Rooftree::Server::Session& /*session*/, ApiRequest request)
{
    std::vector<std::string> providers;
    for (OAuthProvider const& provider : sWorld->GetSettings().OAuthProviders)
        if (provider.IsEnabled())
            providers.push_back(provider.Name);
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateOAuthProviders(builder, StringList(builder, providers)));
    request->Respond(builder);
    co_return;
}

void BlogSession::HandleOAuthStart(Request const& request)
{
    std::string providerName(request->GetPathParameter("provider"));
    if (!sWorld->GetSettings().FindOAuthProvider(providerName))
        return request->RespondError(http::status::not_found, "not_found");

    Optional<boost::json::object> body = Rooftree::Server::ParseJsonObject(request->GetBody());
    if (!body)
        return request->RespondError(http::status::bad_request, "invalid_json");

    RequestFields fields(*body);
    std::string lang = fields.Lang("lang", false);
    std::string next = fields.Text("next", 0, 200, false);
    Optional<bool> link = fields.Bool("link");
    if (!fields.IsValid())
        return RespondValidation(request, fields.TakeErrors());
    if (lang.empty())
        lang = "zh";

    // Leaves for the provider: the address to go to, and a cookie that ties the callback to this browser
    auto start = [request, providerName, lang, next](uint32 linkUserId)
    {
        OAuthProvider const* provider = sWorld->GetSettings().FindOAuthProvider(providerName);
        OAuthPkce pkce = OAuthPkce::Generate();
        std::string fallback = linkUserId ? Rooftree::StringFormat("/{}/account", lang) : Rooftree::StringFormat("/{}", lang);
        std::string state = sWorld->StartOAuthFlow({ .Provider = providerName, .LinkUserId = linkUserId, .Lang = lang,
            .Next = SafeNext(next, fallback), .CodeVerifier = pkce.Verifier, .Expires = TimePoint::clock::now() + OAuthFlowLifetime });
        if (state.empty())
            return request->RespondError(http::status::service_unavailable, "busy");

        SetOAuthCookie(request, OAuthStateCookie, state, OAuthFlowLifetime);
        request->RespondJson(http::status::ok, { { "url", BuildOAuthAuthorizeUrl(*provider, OAuthRedirectUri(providerName), state, pkce.Challenge) } });
    };

    if (!link.value_or(false))
    {
        if (CheckOrigin(request))
            start(0);
        return;
    }

    RequireUser(request, [request, start](AuthInfo const* auth)
    {
        start(auth->User.Id);
    });
}

Rooftree::Task<void> BlogSession::HandleOAuthCallback(Request request)
{
    std::string providerName(request->GetPathParameter("provider"));
    std::string state = request->GetQueryParameter("state").value_or("");
    Optional<std::string_view> cookie = request->GetCookie(OAuthStateCookie);
    SetOAuthCookie(request, OAuthStateCookie, "", 0s);

    // The state must be the one this browser left with: someone else's callback link doesn't log it in
    Optional<BlogWorld::OAuthFlow> flow;
    if (!state.empty() && cookie && Rooftree::Server::ConstantTimeEquals(state, *cookie))
        flow = sWorld->TakeOAuthFlow(state);
    if (!flow || flow->Provider != providerName)
    {
        RT_LOG_INFO("blog.auth", "{}: {} callback without a matching state", request->GetClientAddress().to_string(), providerName);
        co_return RespondRedirect(request, sWorld->GetSettings().PublicOrigin + "/zh/login?oauth_error=oauth_state&provider=" + providerName);
    }

    OAuthProvider const* provider = sWorld->GetSettings().FindOAuthProvider(providerName);
    std::string code = request->GetQueryParameter("code").value_or("");
    if (!provider || code.empty() || code.size() > 512)
    {
        // The user said no at the provider (error=access_denied), or the provider was turned off meanwhile
        co_return RedirectOAuthError(request, *flow, "oauth_cancelled");
    }

    struct Result
    {
        std::string Error;
        std::string Detail;         // for the log
        Optional<UserRow> User;
        OAuthIdentity Identity;
        bool Created = false;
        NewAuthSession Session;
    };

    Seconds lifetime = sWorld->GetSettings().SessionLifetime;
    std::string ip = request->GetClientAddress().to_string();
    std::string userAgent = GetUserAgent(request);
    bool ipBanned = sWorld->IsIpBanned(request->GetClientAddress());
    std::string uploadDirectory = sWorld->GetSettings().UploadDirectory;
    Result result = co_await OnWorker([provider = *provider, redirectUri = OAuthRedirectUri(providerName), code, flow = *flow, ip, userAgent, lifetime, ipBanned,
        uploadDirectory]
    {
        Result result;
        Optional<OAuthIdentity> identity = WorkerFetchOAuthIdentity(provider, redirectUri, code, flow.CodeVerifier, result.Detail);
        if (!identity)
        {
            result.Error = "oauth_failed";
            return result;
        }
        result.Identity = *identity;

        Optional<UserRow> owner = WorkerFindUserByIdentity(provider.Name, identity->ProviderUserId);
        if (owner)
            WorkerRefreshIdentity(provider.Name, *identity);
        if (flow.LinkUserId)
        {
            if (owner && owner->Id != flow.LinkUserId)
                result.Error = "identity_taken";
            else if (!owner && !WorkerInsertIdentity(flow.LinkUserId, provider.Name, *identity))
                result.Error = "already_linked";
            return result;
        }

        if (!owner)
        {
            if (identity->Email.empty())
                result.Error = "oauth_no_email";
            else if (FindUser(BLOG_SEL_USER_BY_EMAIL, identity->Email))
                result.Error = "email_taken";
            else if (ipBanned)
                result.Error = "ip_banned";
            if (!result.Error.empty())
                return result;

            owner = WorkerCreateOAuthUser(*identity, flow.Lang);
            if (!owner || !WorkerInsertIdentity(owner->Id, provider.Name, *identity))
            {
                result.Error = "email_taken";
                return result;
            }
            result.Created = true;

            // A new account starts with the provider's picture; without one if it doesn't come
            std::string error;
            if (Optional<std::string> picture = identity->AvatarUrl.empty() ? Optional<std::string>()
                : WorkerFetchProviderAvatar(provider.Name, identity->AvatarUrl, error))
                owner->Avatar = WorkerSaveAvatar(owner->Id, *picture, uploadDirectory);
        }

        if (owner->IsBanned())
            result.Error = "banned";
        else if (!owner->TotpSecret)
            result.Session = WorkerCreateAuthSession(owner->Id, lifetime, ip, userAgent);
        result.User = std::move(owner);
        return result;
    });
    std::string const& origin = sWorld->GetSettings().PublicOrigin;
    if (!result.Error.empty())
    {
        RT_LOG_INFO("blog.auth", "{}: {} login of {} refused: {}{}{}", request->GetClientAddress().to_string(), flow->Provider,
            result.Identity.Login, result.Error, result.Detail.empty() ? "" : ", ", result.Detail);
        co_return RedirectOAuthError(request, *flow, result.Error);
    }

    if (flow->LinkUserId)
    {
        RT_LOG_INFO("blog.auth", "User {} linked {} {}", flow->LinkUserId, flow->Provider, result.Identity.Login);
        co_return RespondRedirect(request, Rooftree::StringFormat("{}{}?oauth_linked={}", origin, flow->Next, flow->Provider));
    }

    UserRow const& user = *result.User;
    if (result.Created)
        RT_LOG_INFO("blog.auth", "{} registered {} ({}) with {} {}", request->GetClientAddress().to_string(), user.Username, user.Id,
            flow->Provider, result.Identity.Login);

    // Two-step login: the code on the login page, the ticket in a cookie
    if (user.TotpSecret)
    {
        std::string ticket = sWorld->AddOAuthTicket({ .UserId = user.Id, .Next = flow->Next,
            .Expires = TimePoint::clock::now() + OAuthTicketLifetime });
        SetOAuthCookie(request, OAuthTicketCookie, ticket, OAuthTicketLifetime);
        co_return RespondRedirect(request, Rooftree::StringFormat("{}/{}/login?oauth=totp&provider={}", origin, flow->Lang, flow->Provider));
    }

    RT_LOG_INFO("blog.auth", "{} logged in as {} ({}) with {}", request->GetClientAddress().to_string(), user.Username, user.Id, flow->Provider);
    static_cast<BlogSession&>(*this).StartAuthSession(request, user, result.Session.Token, result.Session.Hash,
        result.Session.CsrfToken, result.Session.ExpiresAt);
    RespondRedirect(request, origin + flow->Next);
}

Rooftree::Task<void> BlogSession::HandleOAuthTotp(Request request)
{
    if (!CheckOrigin(request))
        co_return;

    Optional<boost::json::object> body = Rooftree::Server::ParseJsonObject(request->GetBody());
    if (!body)
        co_return request->RespondError(http::status::bad_request, "invalid_json");

    RequestFields fields(*body);
    std::string totp = fields.Text("totp", 6, 6);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    std::string ticket(request->GetCookie(OAuthTicketCookie).value_or(""));
    BlogWorld::OAuthTicket* pending = ticket.empty() ? nullptr : sWorld->FindOAuthTicket(ticket);
    if (!pending)
        co_return request->RespondError(http::status::unauthorized, "oauth_expired");

    // A few tries per ticket; then the login starts over at the provider
    if (++pending->Attempts > OAuthTicketAttempts)
    {
        sWorld->RemoveOAuthTicket(ticket);
        SetOAuthCookie(request, OAuthTicketCookie, "", 0s);
        co_return request->RespondError(http::status::unauthorized, "oauth_expired");
    }

    struct Result
    {
        bool Ok = false;
        Optional<UserRow> User;
        NewAuthSession Session;
    };

    uint32 userId = pending->UserId;
    std::string next = pending->Next;
    Seconds lifetime = sWorld->GetSettings().SessionLifetime;
    std::string ip = request->GetClientAddress().to_string();
    std::string userAgent = GetUserAgent(request);
    Result result = co_await OnWorker([=]
    {
        Result result;
        result.User = FindUserById(userId);
        if (!result.User || !result.User->TotpSecret || result.User->IsBanned())
            return result;

        Optional<int64> step = VerifyTotp(*result.User->TotpSecret, totp, result.User->TotpLastStep);
        if (!step)
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_STEP);
        stmt->setInt64(0, *step);
        stmt->setUInt32(1, userId);
        BlogDatabase.DirectExecute(stmt);
        result.User->TotpLastStep = step;
        result.Session = WorkerCreateAuthSession(userId, lifetime, ip, userAgent);
        result.Ok = true;
        return result;
    });
    if (!result.Ok)
        co_return request->RespondError(http::status::unauthorized, "totp_invalid");

    sWorld->RemoveOAuthTicket(ticket);
    SetOAuthCookie(request, OAuthTicketCookie, "", 0s);
    RT_LOG_INFO("blog.auth", "{} logged in as {} ({}) with a provider and a code", request->GetClientAddress().to_string(),
        result.User->Username, result.User->Id);
    std::string csrf = StartAuthSession(request, *result.User, result.Session.Token,
        result.Session.Hash, result.Session.CsrfToken, result.Session.ExpiresAt);
    // And the page the login started from
    boost::json::object me = MeJson(*result.User, csrf);
    me["next"] = next;
    request->RespondJson(http::status::ok, me);
}

Rooftree::Task<void> ListIdentities(Rooftree::Server::Session& session, ApiRequest request)
{
    struct Item
    {
        std::string Provider, Login;
        SystemTimePoint CreatedAt;
        bool HasAvatar;
    };

    std::vector<Item> identities = co_await session.OnWorker([userId = request->GetAuth().User.Id]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_IDENTITIES);
        stmt->setUInt32(0, userId);
        std::vector<Item> identities;
        if (PreparedQueryResult result = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* row = result->Fetch();
                identities.push_back({ row[0].GetString(), row[1].GetString(), row[2].GetDate(), row[3].GetInt64() != 0 });
            } while (result->NextRow());
        }
        return identities;
    });
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Reader::Identity>> items;
    for (Item const& identity : identities)
        items.push_back(Reader::CreateIdentity(builder, builder.CreateString(identity.Provider), builder.CreateString(identity.Login),
            TimeString(builder, identity.CreatedAt), identity.HasAvatar));
    builder.Finish(Reader::CreateIdentitiesList(builder, builder.CreateVector(items)));
    request->Respond(builder);
}

Rooftree::Task<void> DeleteIdentity(Rooftree::Server::Session& session, ApiRequest request)
{
    std::string provider = ReadText(request->Get<Reader::ProviderRequest>().provider());
    uint32 userId = request->GetAuth().User.Id;
    enum class Outcome { Ok, NotFound, LastLogin };
    Outcome outcome = co_await session.OnWorker([userId, provider]
    {
        Optional<UserRow> user = FindUserById(userId);
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_IDENTITIES);
        stmt->setUInt32(0, userId);
        PreparedQueryResult result = BlogDatabase.Query(stmt);
        std::size_t count = 0;
        bool found = false;
        if (result)
        {
            do
            {
                ++count;
                found |= result->Fetch()[0].GetString() == provider;
            } while (result->NextRow());
        }

        if (!user || !found)
            return Outcome::NotFound;
        // Without a password this is how the account logs in
        if (!user->HasPassword && count == 1)
            return Outcome::LastLogin;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_IDENTITY);
        stmt->setUInt32(0, userId);
        stmt->setString(1, provider);
        BlogDatabase.DirectExecute(stmt);
        return Outcome::Ok;
    });
    if (outcome == Outcome::NotFound)
        co_return request->RespondError(http::status::not_found, "not_found");
    if (outcome == Outcome::LastLogin)
        co_return request->RespondError(http::status::conflict, "last_login_method");

    RT_LOG_INFO("blog.auth", "User {} unlinked {}", userId, provider);
    RespondOk(request);
}

namespace
{
// After the picture changed: the cached login sees it too, the page gets the account
void RespondAvatarChanged(ApiRequest const& request, Optional<UserRow> const& user)
{
    if (!user)
        return RespondInternalError(request);

    RT_LOG_INFO("blog.auth", "User {} changed the picture to {}", user->Id, user->Avatar.empty() ? "its initials" : user->Avatar);
    sWorld->UpdateCachedUser(user->Id, [&](UserRow& cached) { cached.Avatar = user->Avatar; });
    RespondProfile(request, *user);
}
}

Rooftree::Task<void> UploadAvatar(Rooftree::Server::Session& session, ApiRequest request)
{
    UserRow const& user = request->GetAuth().User;
    if (user.IsDenied(DenyAvatar))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    flatbuffers::Vector<uint8> const* file = request->Get<Reader::AvatarUploadRequest>().file();
    if (!file || !file->size())
        co_return RespondValidation(request, { { "file", "required" } });
    if (file->size() > MaxAvatarSize)
        co_return RespondValidation(request, { { "file", "too_long" } });
    std::string data(reinterpret_cast<char const*>(file->data()), file->size());
    if (AvatarExtension(data).empty())
        co_return RespondValidation(request, { { "file", "unsupported_type" } });
    if (!sWorld->GetAvatarLimiter().Allow(user.Id))
        co_return request->RespondError(http::status::too_many_requests, "too_many_avatar_changes");

    Optional<UserRow> changed = co_await session.OnWorker([userId = user.Id, data = std::move(data), directory = sWorld->GetSettings().UploadDirectory]
    {
        if (WorkerSaveAvatar(userId, data, directory).empty())
        {
            RT_LOG_ERROR("blog.auth", "Could not write the picture of user {} into Blog.Uploads.Directory", userId);
            return Optional<UserRow>();
        }
        return FindUserById(userId);
    });
    RespondAvatarChanged(request, changed);
}

Rooftree::Task<void> UseProviderAvatar(Rooftree::Server::Session& session, ApiRequest request)
{
    std::string provider = ReadText(request->Get<Reader::ProviderRequest>().provider());
    if (provider != "github" && provider != "google")
        co_return RespondNotFound(request);

    UserRow const& user = request->GetAuth().User;
    if (user.IsDenied(DenyAvatar))
        co_return request->RespondError(http::status::forbidden, "not_allowed");

    if (!sWorld->GetAvatarLimiter().Allow(user.Id))
        co_return request->RespondError(http::status::too_many_requests, "too_many_avatar_changes");

    struct Result
    {
        std::string Error;
        std::string Detail;     // for the log
        Optional<UserRow> User;
    };

    Result result = co_await session.OnWorker([userId = user.Id, provider, directory = sWorld->GetSettings().UploadDirectory]
    {
        Result result;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_IDENTITY_AVATAR_URL);
        stmt->setUInt32(0, userId);
        stmt->setString(1, provider);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        std::string url = row ? row->Fetch()[0].GetStringOrNull().value_or("") : "";
        if (url.empty())
        {
            result.Error = "no_avatar";
            return result;
        }

        Optional<std::string> picture = WorkerFetchProviderAvatar(provider, url, result.Detail);
        if (!picture)
            result.Error = "avatar_unavailable";
        else if (WorkerSaveAvatar(userId, *picture, directory).empty())
            result.Error = "internal";
        else
            result.User = FindUserById(userId);
        return result;
    });
    if (result.Error == "no_avatar")
        co_return request->RespondError(http::status::conflict, "no_avatar");
    if (result.Error == "avatar_unavailable")
    {
        RT_LOG_INFO("blog.auth", "The {} picture didn't come: {}", provider, result.Detail);
        co_return request->RespondError(http::status::bad_gateway, "avatar_unavailable");
    }
    RespondAvatarChanged(request, result.User);
}

Rooftree::Task<void> DeleteAvatar(Rooftree::Server::Session& session, ApiRequest request)
{
    uint32 userId = request->GetAuth().User.Id;
    if (!sWorld->GetAvatarLimiter().Allow(userId))
        co_return request->RespondError(http::status::too_many_requests, "too_many_avatar_changes");

    Optional<UserRow> user = co_await session.OnWorker([userId, directory = sWorld->GetSettings().UploadDirectory]
    {
        WorkerRemoveAvatar(userId, directory);
        return FindUserById(userId);
    });
    RespondAvatarChanged(request, user);
}
}
