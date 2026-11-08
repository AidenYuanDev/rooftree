/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Blog.* of blog.yaml, and the Sessions and AntiDos of Server.*. LoadFromConfig runs at startup and
 * again on SIGHUP (between two updates, on the main thread); the network settings (addresses, ports,
 * threads) are only read at startup.
 */

#ifndef BLOG_SETTINGS_H
#define BLOG_SETTINGS_H

#include "AntiDos.h"
#include "Duration.h"
#include "Mailer.h"
#include "OAuth.h"
#include "SessionMgr.h"
#include <string>
#include <vector>

class Config;

namespace Blog
{
struct BlogSettings
{
    std::string SiteName = "Blog";
    // Where readers open the site: mail links, the RSS feed and the Origin check use it
    std::string PublicOrigin = "http://localhost:5173";
    std::vector<std::string> AllowedOrigins;    // PublicOrigin by default

    // Auth
    std::string CookieName = "blog_session";
    bool CookieSecure = true;
    Seconds SessionLifetime = 30 * 24h;
    Seconds SessionRenewAfter = 24h;            // extend the session when it was last seen longer ago
    Seconds AuthCacheLifetime = 5min;           // how long a cached session is believed without the database
    Seconds EmailVerificationLifetime = 24h;
    Seconds PasswordResetLifetime = 1h;
    uint32 Argon2Iterations = 2;
    uint32 Argon2MemoryCost = 19 * 1024;        // KiB, OWASP's minimum for Argon2id
    std::string TotpIssuer;                     // SiteName by default
    Seconds MailInterval = 60s;                 // between two verification, reset or subscription mails to one address
    uint32 LoginFailuresPerAccount = 10;        // within LoginFailureWindow, then the account waits
    Seconds LoginFailureWindow = 15min;

    // The picture captcha of the requests that mail someone: registering, subscribing, a reset
    bool CaptchaEnabled = true;
    Seconds CaptchaLifetime = 10min;

    // Comments (M4)
    uint32 MaxCommentsPerUserPerPost = 1024;
    Seconds CommentEditWindow = 5min;
    uint32 CommentsPerUser = 3;                 // within CommentWindow
    Seconds CommentWindow = 1min;

    // Accounts' pictures: changes per account (an upload, a provider's, back to the initials)
    uint32 AvatarChangesPerUser = 10;           // within AvatarWindow
    Seconds AvatarWindow = 1h;

    // Uploads (M4)
    std::string UploadDirectory = "uploads";
    uint32 MaxUploadSize = 10 * 1024 * 1024;

    // Accounts' drives (Drive.h): files under DriveDirectory, bytes per account (0: nothing until the
    // admin gives it a size), per file, and what the disk keeps free whatever the quotas say
    std::string DriveDirectory = "drive";
    uint64 DriveQuota = 0;
    uint64 DriveAdminQuota = 0;
    uint64 DriveMaxFileSize = 2ull << 30;
    uint64 DriveMinFreeSpace = 2ull << 30;
    // Days a deleted file stays in the trash, unless the admin gave the account its own (0: none)
    uint32 DriveTrashDays = 1;

    // The prerendered site, answered from memory (StaticHandlers.cpp); empty: none (development, the
    // vite dev server has the pages)
    std::string StaticDirectory;

    // Rebuilding the prerendered pages (M4)
    std::string RebuildCommand;                 // empty: never
    Seconds RebuildDebounce = 10s;

    // The status page (ServerStatus.h): a sample this often
    Seconds StatusSampleInterval = 60s;
    // Where the statistics' day begins: minutes east of UTC (China's by default)
    Minutes StatsDayOffset = 480min;

    Rooftree::Server::MailSettings Mail;
    std::string MailOutboxDirectory;            // set: mails are written there as .eml instead of sent

    // GitHub and Google logins, all of them; the ones without a client id and secret are off
    std::vector<OAuthProvider> OAuthProviders;
    // An enabled provider by name, null if there is none
    OAuthProvider const* FindOAuthProvider(std::string_view name) const;
    uint32 NewsletterMailsPerMinute = 60;

    // Server.WorldTick: the game's world, every WorldTick at most WorldTickMaxCatchUp ticks in one update
    Milliseconds WorldTick = 50ms;
    uint32 WorldTickMaxCatchUp = 3;

    Rooftree::Server::SessionMgrSettings Sessions;
    Rooftree::Server::AntiDosSettings AntiDos;

    // False (and logged) if a setting is invalid
    bool LoadFromConfig(Config const& config);
};

// The path of a server setting (key "Http.Port"): Server.<key>, the frame's settings shared with the
// game. A config written before they moved has them as Blog.<key>, still read, with a warning once
std::string ServerKey(Config const& config, std::string_view key);
}

#endif // BLOG_SETTINGS_H
