/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "BlogSettings.h"
#include "Config.h"
#include "Drive.h"
#include "Log.h"
#include <algorithm>
#include <mutex>
#include <set>

namespace Blog
{
std::string ServerKey(Config const& config, std::string_view key)
{
    std::string path = "Server." + std::string(key);
    if (config.GetNode(path))
        return path;

    std::string old = "Blog." + std::string(key);
    if (!config.GetNode(old))
        return path;

    static std::mutex warnedLock;
    static std::set<std::string, std::less<>> warned;
    std::scoped_lock lock(warnedLock);
    if (warned.insert(old).second)
        RT_LOG_WARN("blog", "{} is {} now (blog.yaml.dist, SERVER SETTINGS); read where it is for now", old, path);
    return old;
}

bool BlogSettings::LoadFromConfig(Config const& config)
{
    BlogSettings settings;

    settings.SiteName = config.Get<std::string>("Blog.SiteName", settings.SiteName);
    settings.PublicOrigin = config.Get<std::string>("Blog.PublicOrigin", settings.PublicOrigin);
    while (settings.PublicOrigin.ends_with('/'))
        settings.PublicOrigin.pop_back();
    settings.AllowedOrigins = config.Get<std::vector<std::string>>("Blog.AllowedOrigins", {});
    if (settings.AllowedOrigins.empty())
        settings.AllowedOrigins.push_back(settings.PublicOrigin);

    settings.CookieName = config.Get<std::string>("Blog.Auth.CookieName", settings.CookieName);
    settings.CookieSecure = config.Get<bool>("Blog.Auth.CookieSecure", settings.CookieSecure);
    settings.SessionLifetime = Seconds(config.Get<uint32>("Blog.Auth.SessionLifetime", uint32(settings.SessionLifetime.count())));
    settings.SessionRenewAfter = Seconds(config.Get<uint32>("Blog.Auth.SessionRenewAfter", uint32(settings.SessionRenewAfter.count())));
    settings.AuthCacheLifetime = Seconds(config.Get<uint32>("Blog.Auth.CacheLifetime", uint32(settings.AuthCacheLifetime.count())));
    settings.EmailVerificationLifetime = Seconds(config.Get<uint32>("Blog.Auth.EmailVerificationLifetime", uint32(settings.EmailVerificationLifetime.count())));
    settings.PasswordResetLifetime = Seconds(config.Get<uint32>("Blog.Auth.PasswordResetLifetime", uint32(settings.PasswordResetLifetime.count())));
    settings.Argon2Iterations = config.Get<uint32>("Blog.Auth.Argon2.Iterations", settings.Argon2Iterations);
    settings.Argon2MemoryCost = config.Get<uint32>("Blog.Auth.Argon2.MemoryCost", settings.Argon2MemoryCost);
    settings.TotpIssuer = config.Get<std::string>("Blog.Auth.TotpIssuer", "");
    if (settings.TotpIssuer.empty())
        settings.TotpIssuer = settings.SiteName;
    settings.MailInterval = Seconds(config.Get<uint32>("Blog.Auth.MailInterval", uint32(settings.MailInterval.count())));
    settings.LoginFailuresPerAccount = config.Get<uint32>("Blog.Auth.LoginFailuresPerAccount", settings.LoginFailuresPerAccount);
    settings.LoginFailureWindow = Seconds(config.Get<uint32>("Blog.Auth.LoginFailureWindow", uint32(settings.LoginFailureWindow.count())));

    settings.CaptchaEnabled = config.Get<bool>("Blog.Captcha.Enable", settings.CaptchaEnabled);
    settings.CaptchaLifetime = Seconds(std::max(1u, config.Get<uint32>("Blog.Captcha.Lifetime", uint32(settings.CaptchaLifetime.count()))));

    settings.MaxCommentsPerUserPerPost = config.Get<uint32>("Blog.Comments.MaxPerUserPerPost", settings.MaxCommentsPerUserPerPost);
    settings.CommentEditWindow = Seconds(config.Get<uint32>("Blog.Comments.EditWindow", uint32(settings.CommentEditWindow.count())));
    settings.CommentsPerUser = config.Get<uint32>("Blog.Comments.PerUser.MaxCount", settings.CommentsPerUser);
    settings.CommentWindow = Seconds(config.Get<uint32>("Blog.Comments.PerUser.Window", uint32(settings.CommentWindow.count())));
    settings.AvatarChangesPerUser = config.Get<uint32>("Blog.Avatars.PerUser.MaxCount", settings.AvatarChangesPerUser);
    settings.AvatarWindow = Seconds(config.Get<uint32>("Blog.Avatars.PerUser.Window", uint32(settings.AvatarWindow.count())));

    settings.UploadDirectory = config.Get<std::string>("Blog.Uploads.Directory", settings.UploadDirectory);
    settings.MaxUploadSize = config.Get<uint32>("Blog.Uploads.MaxSize", settings.MaxUploadSize);

    settings.DriveDirectory = config.Get<std::string>("Blog.Drive.Directory", settings.DriveDirectory);
    settings.DriveQuota = config.Get<uint64>("Blog.Drive.Quota", settings.DriveQuota);
    settings.DriveAdminQuota = config.Get<uint64>("Blog.Drive.AdminQuota", settings.DriveAdminQuota);
    settings.DriveMaxFileSize = config.Get<uint64>("Blog.Drive.MaxFileSize", settings.DriveMaxFileSize);
    settings.DriveMinFreeSpace = config.Get<uint64>("Blog.Drive.MinFreeSpace", settings.DriveMinFreeSpace);
    settings.DriveTrashDays = std::min<uint32>(config.Get<uint32>("Blog.Drive.TrashDays", settings.DriveTrashDays), Drive::MaxTrashDays);

    settings.NewsletterMailsPerMinute = std::max(1u, config.Get<uint32>("Blog.Newsletter.MailsPerMinute", settings.NewsletterMailsPerMinute));
    settings.StaticDirectory = config.Get<std::string>("Blog.Static.Directory", "");
    settings.RebuildCommand = config.Get<std::string>("Blog.Rebuild.Command", "");
    settings.RebuildDebounce = Seconds(config.Get<uint32>("Blog.Rebuild.Debounce", uint32(settings.RebuildDebounce.count())));
    settings.StatsDayOffset = Minutes(std::clamp(config.Get<int32>("Blog.Stats.DayOffset", int32(settings.StatsDayOffset.count())), -720, 840));
    settings.StatusSampleInterval = Seconds(std::max(1u, config.Get<uint32>("Blog.Status.SampleInterval", uint32(settings.StatusSampleInterval.count()))));

    settings.Mail = Rooftree::Server::MailSettings::FromConfig(config, "Blog.Mail");
    settings.MailOutboxDirectory = config.Get<std::string>("Blog.Mail.OutboxDirectory", "");
    settings.OAuthProviders = OAuthProvider::FromConfig(config);

    settings.WorldTick = Milliseconds(config.Get<uint32>(ServerKey(config, "WorldTick"), uint32(settings.WorldTick.count())));
    settings.WorldTickMaxCatchUp = std::max(1u, config.Get<uint32>(ServerKey(config, "WorldTickMaxCatchUp"), settings.WorldTickMaxCatchUp));
    settings.Sessions = Rooftree::Server::SessionMgrSettings::FromConfig(config, ServerKey(config, "Sessions"));
    // blog.yaml.dist's, for a config written before a type had its limit. The drive's are far above what
    // its pages send, so its users get full speed. The host itself is exempt: the prerender reads every
    // post through the API (deploy/rebuild.sh). An address refused 1000 times in 10 s, which no reader's
    // browser comes near, is blocked for KickDuration
    Rooftree::Server::AntiDosSettings antiDosDefaults;
    antiDosDefaults.KickDuration = 10min;
    antiDosDefaults.BlockAfterRejections = 1000;
    antiDosDefaults.BlockAfterWindow = 10s;
    antiDosDefaults.Exempt = { boost::asio::ip::make_address("127.0.0.1"), boost::asio::ip::make_address("::1") };
    antiDosDefaults.Limits = {
        { "auth.captcha", { .Window = 60s, .MaxCount = 20 } },
        { "auth.oauth", { .Window = 60s, .MaxCount = 20 } },
        { "auth.register", { .Window = 1h, .MaxCount = 20 } },
        { "auth.login", { .Window = 60s, .MaxCount = 10 } },
        { "auth.token", { .Window = 60s, .MaxCount = 10 } },
        { "auth.mail", { .Window = 1h, .MaxCount = 10 } },
        { "auth.password", { .Window = 60s, .MaxCount = 5 } },
        { "auth.totp", { .Window = 60s, .MaxCount = 10 } },
        { "auth.session", { .Window = 10s, .MaxCount = 200 } },
        { "auth.profile", { .Window = 60s, .MaxCount = 60 } },
        { "posts.read", { .Window = 10s, .MaxCount = 200 } },
        { "posts.search", { .Window = 60s, .MaxCount = 30 } },
        { "comments.read", { .Window = 10s, .MaxCount = 200 } },
        { "comments.write", { .Window = 60s, .MaxCount = 20 } },
        { "reactions.write", { .Window = 60s, .MaxCount = 60 } },
        { "friends.apply", { .Window = 1h, .MaxCount = 5 } },
        { "chat", { .Window = 10s, .MaxCount = 200 } },
        { "chat.write", { .Window = 60s, .MaxCount = 30 } },
        { "ws.page", { .Window = 60s, .MaxCount = 30 } },
        { "ws.doc", { .Window = 10s, .MaxCount = 300 } },
        { "views", { .Window = 60s, .MaxCount = 30 } },
        { "drive", { .Window = 10s, .MaxCount = 1000 } },
        { "drive.share", { .Window = 60s, .MaxCount = 300 } },
    };
    Optional<Rooftree::Server::AntiDosSettings> antiDos = Rooftree::Server::AntiDosSettings::FromConfig(config, ServerKey(config, "AntiDos"), std::move(antiDosDefaults));
    if (!antiDos)
        return false;
    settings.AntiDos = std::move(*antiDos);

    if (settings.Mail.From.empty())
    {
        RT_LOG_ERROR("blog", "Blog.Mail.From is not set, mails need a sender");
        return false;
    }

    if (settings.Argon2Iterations < 1 || settings.Argon2MemoryCost < 8 * 1024)
    {
        RT_LOG_ERROR("blog", "Blog.Auth.Argon2: Iterations must be at least 1 and MemoryCost at least 8192 KiB");
        return false;
    }

    *this = std::move(settings);
    return true;
}

OAuthProvider const* BlogSettings::FindOAuthProvider(std::string_view name) const
{
    auto itr = std::ranges::find_if(OAuthProviders, [&](OAuthProvider const& provider) { return provider.Name == name && provider.IsEnabled(); });
    return itr != OAuthProviders.end() ? &*itr : nullptr;
}
}
