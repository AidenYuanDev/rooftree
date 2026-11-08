/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Sends mail over SMTP with libcurl: smtps://host:465 (TLS from the start) or smtp://host:587, where
 * STARTTLS is required unless RequireTls is off (a local test server such as Mailpit). Send() blocks
 * for the whole SMTP conversation, so it runs on a worker (Session::OnWorker), never on the main
 * thread.
 *
 * Without a Url nothing is sent and the whole mail is logged instead ("server.mail", info), which is
 * what a development setup wants: the verification link is in the log.
 *
 * Messages are multipart/alternative with a text and an HTML part, both UTF-8 and base64, and an
 * RFC 2047 encoded subject, so Chinese text survives every relay.
 */

#ifndef ROOFTREE_MAILER_H
#define ROOFTREE_MAILER_H

#include "Define.h"
#include "Duration.h"
#include <ctime>
#include <string>
#include <utility>
#include <vector>

class Config;

namespace Rooftree::Server
{
struct MailSettings
{
    std::string Url;            // empty: log mails instead of sending them
    std::string Username;
    std::string Password;
    std::string From;           // address
    std::string FromName;
    bool RequireTls = true;
    Seconds Timeout = 30s;

    // <path>.Url, .Username, .Password, .From, .FromName, .RequireTls, .Timeout (seconds)
    static MailSettings FromConfig(Config const& config, std::string const& path);
};

struct MailMessage
{
    std::string To;
    std::string Subject;
    std::string TextBody;
    std::string HtmlBody;       // optional
    // More header fields, e.g. List-Unsubscribe; plain ASCII, without line breaks
    std::vector<std::pair<std::string, std::string>> Headers;
};

class Mailer
{
public:
    explicit Mailer(MailSettings settings);

    MailSettings const& GetSettings() const { return _settings; }

    // Blocking. False with error set if the mail was not accepted
    bool Send(MailMessage const& message, std::string* error = nullptr) const;

    // The RFC 5322 message as sent, without the SMTP envelope. Empty if a header would contain a line
    // break (header injection) or an address is empty
    static std::string BuildMessage(MailSettings const& settings, MailMessage const& message, std::time_t date, std::string const& messageId);

private:
    MailSettings _settings;
};
}

#endif // ROOFTREE_MAILER_H
