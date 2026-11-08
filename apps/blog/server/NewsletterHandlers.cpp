/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Mail subscriptions. Anyone may subscribe an address: a mail with a link confirms it (double opt-in),
 * so nobody gets mail they didn't ask for. A logged in reader switches it on and off in the account
 * settings; their address is confirmed already. Every newsletter (Newsletter.h) has the address's
 * unsubscribe link, and List-Unsubscribe headers for the mail client's own button: that one POSTs
 * without our Origin, the token alone is enough to unsubscribe.
 *
 * Subscribing an address that's subscribed already answers like a new one, so the form doesn't tell
 * who reads here.
 */

#include "ApiCall.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include "Validation.h"

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr Seconds ConfirmLifetime = 7 * 24h;

std::string ToLower(std::string text)
{
    std::ranges::transform(text, text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

Rooftree::Server::MailMessage ConfirmMail(std::string const& email, std::string const& lang, std::string const& token)
{
    std::string site = sWorld->GetSiteName(lang);
    std::string link = Rooftree::StringFormat("{}/{}/subscribe/confirm?token={}", sWorld->GetSettings().PublicOrigin, lang, token);
    if (lang == "en")
        return { .To = email, .Subject = "Confirm your subscription to " + site,
            .TextBody = Rooftree::StringFormat("Hi,\n\nopen this link to get a mail when something new is published on {}:\n\n{}\n\n"
                "It works for 7 days. If you didn't subscribe, ignore this mail, you won't hear from us again.\n", site, link) };

    return { .To = email, .Subject = "确认订阅" + site,
        .TextBody = Rooftree::StringFormat("你好：\n\n打开下面的链接，{}有新文章时会发邮件告诉你：\n\n{}\n\n链接 7 天内有效。如果不是你订阅的，请忽略这封邮件，我们不会再打扰你。\n",
            site, link) };
}

struct SubscriberRow
{
    uint32 Id = 0;
    std::string Status;
};

Optional<SubscriberRow> WorkerFindSubscriber(std::string const& email)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_SUBSCRIBER_BY_EMAIL);
    stmt->setString(0, email);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row)
        return {};
    return SubscriberRow{ row->Fetch()[0].GetUInt32(), row->Fetch()[1].GetString() };
}

// Worker: the address active (or not) right away, for a logged in reader whose address is confirmed
void WorkerSetSubscribed(std::string const& email, std::string const& lang, bool subscribed)
{
    if (Optional<SubscriberRow> row = WorkerFindSubscriber(email))
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_SUBSCRIBER_STATUS);
        stmt->setString(0, subscribed ? std::string_view("active") : std::string_view("unsubscribed"));
        stmt->setString(1, lang);
        stmt->setUInt32(2, row->Id);
        BlogDatabase.DirectExecute(stmt);
        return;
    }

    if (!subscribed)
        return;

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_SUBSCRIBER);
    stmt->setString(0, email);
    stmt->setString(1, lang);
    stmt->setString(2, std::string_view("active"));
    stmt->setNull(3);
    stmt->setNull(4);
    stmt->setString(5, Rooftree::Server::GenerateToken(32));
    stmt->setDate(6, SystemTimePoint::clock::now());
    BlogDatabase.DirectExecute(stmt);
}
}

Rooftree::Task<void> Subscribe(Session& session, ApiRequest request)
{
    Reader::SubscribeRequest const& in = request->Get<Reader::SubscribeRequest>();
    TableFields fields(request->GetErrors());
    std::string email = ToLower(fields.Email("email", in.email()));
    std::string lang = fields.Lang("lang", in.lang(), false);
    if (lang.empty())
        lang = "zh";
    if (!fields.IsValid() || !CheckCaptcha(fields, in.captchaId(), in.captcha()))
        co_return RespondValidation(request, fields.TakeErrors());

    if (!CheckMailInterval(request, email))
        co_return;

    std::string token = Rooftree::Server::GenerateToken(32);
    std::string hash = Rooftree::Server::Sha256Hex(token);
    bool mail = co_await session.OnWorker([email, lang, hash]
    {
        Optional<SubscriberRow> row = WorkerFindSubscriber(email);
        if (row && row->Status == "active")
            return false;

        SystemTimePoint expires = SystemTimePoint::clock::now() + ConfirmLifetime;
        BlogDatabasePreparedStatement* stmt;
        if (row)
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_SUBSCRIBER_PENDING);
            stmt->setString(0, lang);
            stmt->setString(1, hash);
            stmt->setDate(2, expires);
            stmt->setUInt32(3, row->Id);
        }
        else
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_SUBSCRIBER);
            stmt->setString(0, email);
            stmt->setString(1, lang);
            stmt->setString(2, std::string_view("pending"));
            stmt->setString(3, hash);
            stmt->setDate(4, expires);
            stmt->setString(5, Rooftree::Server::GenerateToken(32));
            stmt->setNull(6);
        }
        BlogDatabase.DirectExecute(stmt);
        return true;
    });
    // Subscribed already: the same answer, no mail
    if (mail)
        sWorld->SendMail(session, ConfirmMail(email, lang, token));
    RespondOk(request);
}

Rooftree::Task<void> ConfirmSubscription(Session& session, ApiRequest request)
{
    TableFields fields(request->GetErrors());
    std::string token = fields.Text("token", request->Get<Reader::TokenRequest>().token(), 1, 64);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    bool confirmed = co_await session.OnWorker([hash = Rooftree::Server::Sha256Hex(token)]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_SUBSCRIBER_BY_CONFIRM);
        stmt->setString(0, hash);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return false;

        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_SUBSCRIBER_CONFIRMED);
        stmt->setUInt32(0, row->Fetch()[0].GetUInt32());
        BlogDatabase.DirectExecute(stmt);
        return true;
    });
    if (!confirmed)
        co_return request->RespondError(http::status::bad_request, "invalid_token");
    RespondOk(request);
}

Rooftree::Task<void> BlogSession::HandleUnsubscribe(Request request)
{
    // ?token= (the mail client's one-click POST) or {"token": ...} (our page)
    std::string token = request->GetQueryParameter("token").value_or("");
    if (token.empty())
    {
        if (Optional<boost::json::object> body = Rooftree::Server::ParseJsonObject(request->GetBody()))
            if (boost::json::value const* value = body->if_contains("token"); value && value->is_string())
                token = std::string(value->as_string());
    }
    if (token.empty() || token.size() > 64)
        co_return RespondValidation(request, { { "token", "required" } });

    Optional<std::string> email = co_await OnWorker([token]() -> Optional<std::string>
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_SUBSCRIBER_BY_TOKEN);
        stmt->setString(0, token);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        if (!row)
            return {};

        std::string email = row->Fetch()[1].GetString();
        stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_SUBSCRIBER_UNSUBSCRIBED);
        stmt->setUInt32(0, row->Fetch()[0].GetUInt32());
        BlogDatabase.DirectExecute(stmt);
        return email;
    });
    if (!email)
        co_return request->RespondError(http::status::bad_request, "invalid_token");
    RT_LOG_INFO("blog.mail", "{} unsubscribed", *email);
    request->RespondJson(http::status::ok, { { "ok", true } });
}

namespace
{
void RespondSubscription(ApiRequest const& request, bool subscribed)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Reader::CreateSubscription(builder, subscribed));
    request->Respond(builder);
}
}

Rooftree::Task<void> GetSubscription(Session& session, ApiRequest request)
{
    bool subscribed = co_await session.OnWorker([email = ToLower(request->GetAuth().User.Email)]
    {
        Optional<SubscriberRow> row = WorkerFindSubscriber(email);
        return row && row->Status == "active";
    });
    RespondSubscription(request, subscribed);
}

Rooftree::Task<void> SetSubscription(Session& session, ApiRequest request)
{
    UserRow const& user = request->GetAuth().User;
    if (!user.EmailVerified)
        co_return request->RespondError(http::status::forbidden, "email_unverified");

    flatbuffers::Optional<bool> subscribed = request->Get<Reader::SubscriptionSetRequest>().subscribed();
    TableFields fields(request->GetErrors());
    if (!subscribed)
        fields.AddError("subscribed", "required");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    bool on = co_await session.OnWorker([email = ToLower(user.Email), lang = user.Locale, on = *subscribed]
    {
        WorkerSetSubscribed(email, lang, on);
        return on;
    });
    RespondSubscription(request, on);
}
}
