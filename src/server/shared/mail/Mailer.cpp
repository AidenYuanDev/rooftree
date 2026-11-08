/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Mailer.h"
#include "Config.h"
#include "Log.h"
#include "SecurityUtils.h"
#include <curl/curl.h>
#include <mutex>

namespace Rooftree::Server
{
namespace
{
constexpr std::string_view Base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string Base64(std::string_view data)
{
    std::string text;
    text.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < data.size(); i += 3)
    {
        uint32 chunk = uint32(uint8(data[i])) << 16;
        if (i + 1 < data.size())
            chunk |= uint32(uint8(data[i + 1])) << 8;
        if (i + 2 < data.size())
            chunk |= uint32(uint8(data[i + 2]));

        text += Base64Alphabet[(chunk >> 18) & 0x3F];
        text += Base64Alphabet[(chunk >> 12) & 0x3F];
        text += i + 1 < data.size() ? Base64Alphabet[(chunk >> 6) & 0x3F] : '=';
        text += i + 2 < data.size() ? Base64Alphabet[chunk & 0x3F] : '=';
    }

    return text;
}

// Base64 body lines, at most 76 characters (RFC 2045)
std::string Base64Body(std::string_view data)
{
    std::string encoded = Base64(data);
    std::string body;
    for (std::size_t i = 0; i < encoded.size(); i += 76)
    {
        body += encoded.substr(i, 76);
        body += "\r\n";
    }

    return body;
}

bool HasLineBreak(std::string_view text)
{
    return text.find_first_of("\r\n") != std::string_view::npos;
}

// RFC 2047 encoded-word for anything that isn't plain printable ASCII
std::string EncodeHeaderText(std::string_view text)
{
    if (std::ranges::all_of(text, [](char c) { return c >= 0x20 && c < 0x7F; }))
        return std::string(text);

    return "=?UTF-8?B?" + Base64(text) + "?=";
}

void InitCurl()
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct UploadState
{
    std::string_view Data;
    std::size_t Offset = 0;
};

std::size_t ReadPayload(char* buffer, std::size_t size, std::size_t count, void* userData)
{
    UploadState* state = static_cast<UploadState*>(userData);
    std::size_t length = std::min(size * count, state->Data.size() - state->Offset);
    std::memcpy(buffer, state->Data.data() + state->Offset, length);
    state->Offset += length;
    return length;
}
}

MailSettings MailSettings::FromConfig(Config const& config, std::string const& path)
{
    MailSettings settings;
    settings.Url = config.Get<std::string>(path + ".Url", "");
    settings.Username = config.Get<std::string>(path + ".Username", "");
    settings.Password = config.Get<std::string>(path + ".Password", "");
    settings.From = config.Get<std::string>(path + ".From", "");
    settings.FromName = config.Get<std::string>(path + ".FromName", "");
    settings.RequireTls = config.Get<bool>(path + ".RequireTls", true);
    settings.Timeout = Seconds(config.Get<uint32>(path + ".Timeout", 30));
    return settings;
}

Mailer::Mailer(MailSettings settings) : _settings(std::move(settings))
{
    InitCurl();
}

std::string Mailer::BuildMessage(MailSettings const& settings, MailMessage const& message, std::time_t date, std::string const& messageId)
{
    if (settings.From.empty() || message.To.empty() || HasLineBreak(settings.From) || HasLineBreak(settings.FromName)
        || HasLineBreak(message.To) || HasLineBreak(message.Subject) || HasLineBreak(messageId))
        return {};
    for (auto const& [name, value] : message.Headers)
        if (name.empty() || name.find(':') != std::string::npos || HasLineBreak(name) || HasLineBreak(value))
            return {};

    std::tm dateTm;
    gmtime_r(&date, &dateTm);
    char dateText[64];
    std::strftime(dateText, sizeof(dateText), "%a, %d %b %Y %H:%M:%S +0000", &dateTm);

    std::string from = settings.FromName.empty() ? "<" + settings.From + ">"
        : EncodeHeaderText(settings.FromName) + " <" + settings.From + ">";

    std::string mail;
    mail += "Date: " + std::string(dateText) + "\r\n";
    mail += "From: " + from + "\r\n";
    mail += "To: <" + message.To + ">\r\n";
    mail += "Subject: " + EncodeHeaderText(message.Subject) + "\r\n";
    mail += "Message-ID: <" + messageId + ">\r\n";
    mail += "MIME-Version: 1.0\r\n";
    for (auto const& [name, value] : message.Headers)
        mail += name + ": " + value + "\r\n";

    if (message.HtmlBody.empty())
    {
        mail += "Content-Type: text/plain; charset=UTF-8\r\n";
        mail += "Content-Transfer-Encoding: base64\r\n\r\n";
        mail += Base64Body(message.TextBody);
        return mail;
    }

    // Base64 never contains '=' followed by '_', so this can't appear in a part
    std::string boundary = "=_" + GenerateToken(18);
    mail += "Content-Type: multipart/alternative; boundary=\"" + boundary + "\"\r\n\r\n";
    mail += "--" + boundary + "\r\n";
    mail += "Content-Type: text/plain; charset=UTF-8\r\n";
    mail += "Content-Transfer-Encoding: base64\r\n\r\n";
    mail += Base64Body(message.TextBody);
    mail += "--" + boundary + "\r\n";
    mail += "Content-Type: text/html; charset=UTF-8\r\n";
    mail += "Content-Transfer-Encoding: base64\r\n\r\n";
    mail += Base64Body(message.HtmlBody);
    mail += "--" + boundary + "--\r\n";
    return mail;
}

bool Mailer::Send(MailMessage const& message, std::string* error) const
{
    std::size_t at = _settings.From.find('@');
    std::string domain = at == std::string::npos ? "localhost" : _settings.From.substr(at + 1);
    std::string mail = BuildMessage(_settings, message, std::time(nullptr), GenerateToken(16) + "@" + domain);
    if (mail.empty())
    {
        if (error)
            *error = "invalid sender, recipient or header";
        return false;
    }

    if (_settings.Url.empty())
    {
        RT_LOG_INFO("server.mail", "Mail.Url is not set, not sending this mail to {}:\n{}\n{}", message.To, message.Subject, message.TextBody);
        return true;
    }

    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl)
    {
        if (error)
            *error = "curl_easy_init failed";
        return false;
    }

    std::string from = "<" + _settings.From + ">";
    std::string to = "<" + message.To + ">";
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> recipients(curl_slist_append(nullptr, to.c_str()), &curl_slist_free_all);

    UploadState upload{ .Data = mail };
    char errorBuffer[CURL_ERROR_SIZE] = {};

    curl_easy_setopt(curl.get(), CURLOPT_URL, _settings.Url.c_str());
    if (!_settings.Username.empty())
    {
        curl_easy_setopt(curl.get(), CURLOPT_USERNAME, _settings.Username.c_str());
        curl_easy_setopt(curl.get(), CURLOPT_PASSWORD, _settings.Password.c_str());
    }
    curl_easy_setopt(curl.get(), CURLOPT_USE_SSL, long(_settings.RequireTls ? CURLUSESSL_ALL : CURLUSESSL_TRY));
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_FROM, from.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_MAIL_RCPT, recipients.get());
    curl_easy_setopt(curl.get(), CURLOPT_READFUNCTION, &ReadPayload);
    curl_easy_setopt(curl.get(), CURLOPT_READDATA, &upload);
    curl_easy_setopt(curl.get(), CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, long(_settings.Timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);

    CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK)
    {
        std::string message = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
        RT_LOG_ERROR("server.mail", "Sending mail to {} failed: {}", to, message);
        if (error)
            *error = std::move(message);
        return false;
    }

    RT_LOG_DEBUG("server.mail", "Sent mail to {}", to);
    return true;
}
}
