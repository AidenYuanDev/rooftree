/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Newsletter.h"
#include "BlogDatabase.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <filesystem>
#include <fstream>

namespace Blog
{
void DeliverMail(Rooftree::Server::MailSettings const& settings, std::string const& outbox, Rooftree::Server::MailMessage const& message)
{
    if (!outbox.empty())
    {
        // Development and tests: the mail as a file, newest name last when sorted
        std::error_code error;
        std::filesystem::create_directories(outbox, error);
        std::string mail = Rooftree::Server::Mailer::BuildMessage(settings, message, std::time(nullptr), Rooftree::Server::GenerateToken(16) + "@outbox");
        std::string name = Rooftree::StringFormat("{}/{:020}-{}.eml", outbox,
            std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
            Rooftree::Server::Sha256Hex(message.To).substr(0, 8));
        std::ofstream(name, std::ios::binary) << mail;
        RT_LOG_INFO("blog.mail", "Wrote the mail to {} into {}", message.To, name);
        return;
    }

    std::string error;
    if (!Rooftree::Server::Mailer(settings).Send(message, &error))
        RT_LOG_ERROR("blog.mail", "Could not send the mail to {}: {}", message.To, error);
}

Newsletter::~Newsletter()
{
    Stop();
}

void Newsletter::Queue(NewsletterIssue issue)
{
    std::lock_guard lock(_lock);
    if (_stopping)
        return;

    _queue.push_back(std::move(issue));
    if (!_thread.joinable())
        _thread = std::thread([this] { Run(); });
    _wake.notify_one();
}

void Newsletter::Stop()
{
    {
        std::lock_guard lock(_lock);
        _stopping = true;
        if (!_queue.empty())
            RT_LOG_WARN("blog.mail", "Shutting down with {} mailings not sent", _queue.size());
        _queue.clear();
    }
    _wake.notify_one();
    if (_thread.joinable())
        _thread.join();
}

void Newsletter::Run()
{
    for (;;)
    {
        NewsletterIssue issue;
        {
            std::unique_lock lock(_lock);
            _wake.wait(lock, [this] { return _stopping || !_queue.empty(); });
            if (_stopping)
                return;
            issue = std::move(_queue.front());
            _queue.pop_front();
        }

        Send(issue);
    }
}

void Newsletter::Send(NewsletterIssue const& issue)
{
    bool post = issue.Kind == "post";
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(post ? BLOG_SEL_POST_TO_MAIL : BLOG_SEL_NOTE_TO_MAIL);
    stmt->setUInt32(0, issue.Id);
    PreparedQueryResult rows = BlogDatabase.Query(stmt);
    if (!rows)
        return;     // mailed already, or not published (anymore)

    struct Text
    {
        std::string Title;
        std::string Summary;
    };
    std::map<std::string, Text> texts;
    std::string slug;
    do
    {
        Field* fields = rows->Fetch();
        slug = fields[0].GetString();
        texts[fields[1].GetString()] = { fields[2].GetString(), fields[3].GetString() };
    } while (rows->NextRow());

    stmt = BlogDatabase.GetPreparedStatement(post ? BLOG_UPD_POST_MAILED : BLOG_UPD_NOTE_MAILED);
    stmt->setUInt32(0, issue.Id);
    BlogDatabase.DirectExecute(stmt);

    std::string path = post ? "/posts/" + slug : Rooftree::StringFormat("/notes/{}", issue.Id);
    Milliseconds pause = Milliseconds(60000 / std::max(issue.MailsPerMinute, 1u));
    uint32 sent = 0;
    PreparedQueryResult subscribers = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_ACTIVE_SUBSCRIBERS));
    if (subscribers)
    {
        do
        {
            {
                std::lock_guard lock(_lock);
                if (_stopping)
                {
                    RT_LOG_WARN("blog.mail", "Mailing {} {} stopped after {} mails", issue.Kind, issue.Id, sent);
                    return;
                }
            }

            Field* fields = subscribers->Fetch();
            std::string email = fields[0].GetString();
            std::string lang = fields[1].GetString();
            std::string token = fields[2].GetString();
            // Only to the readers of its languages: the other site doesn't have it
            if (!texts.contains(lang))
                continue;
            Text const& text = texts.at(lang);
            std::string site = issue.SiteNames.contains(lang) ? issue.SiteNames.at(lang) : std::string();
            std::string link = Rooftree::StringFormat("{}/{}{}", issue.PublicOrigin, lang, path);
            std::string unsubscribe = Rooftree::StringFormat("{}/{}/unsubscribe?token={}", issue.PublicOrigin, lang, token);
            std::string summary = text.Summary.empty() ? std::string() : text.Summary + "\n\n";

            Rooftree::Server::MailMessage message;
            message.To = email;
            if (lang == "en")
            {
                message.Subject = Rooftree::StringFormat("{}: {}", site, text.Title);
                message.TextBody = Rooftree::StringFormat("{}\n\n{}Read it on {}:\n{}\n\n--\nYou subscribed to {}. Unsubscribe: {}\n",
                    text.Title, summary, site, link, site, unsubscribe);
            }
            else
            {
                message.Subject = Rooftree::StringFormat("「{}」{}{}", site, post ? "新文章：" : "新日记：", text.Title);
                message.TextBody = Rooftree::StringFormat("{}\n\n{}阅读全文：\n{}\n\n——\n你订阅了{}的更新。退订：{}\n", text.Title, summary, link, site, unsubscribe);
            }
            // One-click unsubscribing from the mail client (RFC 8058): a POST to the API with the token
            message.Headers = {
                { "List-Unsubscribe", Rooftree::StringFormat("<{}/api/unsubscribe?token={}>", issue.PublicOrigin, token) },
                { "List-Unsubscribe-Post", "List-Unsubscribe=One-Click" },
            };

            DeliverMail(issue.Mail, issue.Outbox, message);
            ++sent;
            std::this_thread::sleep_for(pause);
        } while (subscribers->NextRow());
    }

    RT_LOG_INFO("blog.mail", "Mailed {} {} to {} subscribers", issue.Kind, issue.Id, sent);
}
}
