/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Like the request handlers (AuthHandlers.cpp): the arguments are checked on the main thread, the
 * queries and Argon2 run on a worker (the Worker* functions, which only see their arguments), the
 * caches are updated and the result printed on the main thread again.
 */

#include "ConsoleCommands.h"
#include "ConsoleAdmin.h"
#include "ConsoleApi.h"
#include "Argon2Hash.h"
#include "Avatar.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Drive.h"
#include "CryptoRandom.h"
#include "Field.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "TOTP.h"
#include "TotpSetup.h"
#include "UpdateLoop.h"
#include "Util.h"
#include "Validation.h"
#include <boost/json/parse.hpp>
#include <boost/json/serialize.hpp>
#include <csignal>
#include <fstream>
#include <unistd.h>

using Rooftree::Server::CommandHandler;
using Rooftree::Server::Session;

namespace Blog
{
namespace
{
Rooftree::Server::UpdateLoop* ConsoleUpdateLoop = nullptr;
TimePoint ConsoleStartTime;

constexpr uint32 ListedAccounts = 50;
// A supervisor restarting only on failure restarts this one
constexpr int RestartExitCode = 2;

enum class Outcome
{
    Ok,
    NoSuchAccount,
    PasswordTooShort,
    AdminCantBeBanned,
    HasContent,
    Taken,
    Failed
};

struct AccountResult
{
    Outcome Result = Outcome::Ok;
    UserRow User;
    std::vector<uint8> TotpSecret;
    int64 Posts = 0;
    int64 Uploads = 0;
};

Optional<UserRow> WorkerFindAccount(std::string const& account)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_LOGIN);
    stmt->setString(0, account);
    stmt->setString(1, account);
    PreparedQueryResult result = BlogDatabase.Query(stmt);
    if (!result)
        return {};

    return UserRow::FromFields(result->Fetch());
}

void WorkerDeleteSessions(uint32 userId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_SESSIONS);
    stmt->setUInt32(0, userId);
    BlogDatabase.DirectExecute(stmt);
}

// The common failures; false for Ok
bool PrintFailure(CommandHandler& handler, std::string const& account, AccountResult const& result)
{
    UserRow const& user = result.User;
    switch (result.Result)
    {
        case Outcome::Ok:
            return false;
        case Outcome::NoSuchAccount:
            handler.PSendSysMessage("There is no account {}, 'lookup account' finds one by a part of it", account);
            break;
        case Outcome::PasswordTooShort:
            handler.PSendSysMessage("The password of {} needs at least {} characters", user.Username, user.IsAdmin() ? 12 : 8);
            break;
        case Outcome::AdminCantBeBanned:
            handler.PSendSysMessage("{} is the admin, it can't be banned", user.Username);
            break;
        case Outcome::HasContent:
            handler.PSendSysMessage("{} has {} posts and {} uploads, they would lose their author. Nothing was deleted",
                user.Username, result.Posts, result.Uploads);
            break;
        case Outcome::Taken:
            handler.SendSysMessage("Another account has it already");
            break;
        case Outcome::Failed:
            handler.SendSysMessage("Hashing the password failed");
            break;
    }
    return true;
}

// Finds the account and runs work(result) on a worker, then done(handler, result) on the main
// thread unless work failed. For CommandHandler::Spawn
template<typename Work, typename Done>
Rooftree::Task<void> FindAccountAnd(CommandHandler& handler, std::string account, Work work, Done done)
{
    AccountResult result = co_await handler.GetSession().OnWorker([account, work = std::move(work)]() mutable
    {
        AccountResult result;
        Optional<UserRow> user = WorkerFindAccount(account);
        if (!user)
        {
            result.Result = Outcome::NoSuchAccount;
            return result;
        }

        result.User = std::move(*user);
        work(result);
        return result;
    });
    if (!PrintFailure(handler, account, result))
        done(handler, result);
}

template<typename Work, typename Done>
void WithAccount(CommandHandler& handler, std::string account, Work work, Done done)
{
    handler.Spawn(FindAccountAnd(handler, std::move(account), std::move(work), std::move(done)));
}

std::string Join(std::vector<std::string> const& words, std::size_t first)
{
    std::string joined;
    for (std::size_t i = first; i < words.size(); ++i)
        joined += (i == first ? "" : " ") + words[i];
    return joined;
}

// A ban's end: perm or a time string, 30m, 1d12h. Nothing for neither; perm is PermanentBanTime()
Optional<SystemTimePoint> ParseBanEnd(std::string const& text)
{
    if (text == "perm" || text == "-1")
        return PermanentBanTime();
    if (Optional<Seconds> duration = Rooftree::ParseDuration(text); duration && duration->count())
        return SystemTimePoint::clock::now() + *duration;
    return {};
}

std::string FormatBanEnd(SystemTimePoint end)
{
    return end >= PermanentBanTime() ? "for good" : "until " + FormatTime(end);
}

// A delay in seconds or a time string; nothing for neither
Optional<Seconds> ParseDelay(std::string const& text)
{
    if (!text.empty() && std::ranges::all_of(text, [](char c) { return c >= '0' && c <= '9'; }) && text.size() < 9)
        return Seconds(std::stoul(text));
    if (Optional<Seconds> duration = Rooftree::ParseDuration(text); duration && duration->count())
        return *duration;
    return {};
}

std::string FormatAccount(uint32 id, std::string_view username, std::string_view displayName, std::string_view email, std::string_view role,
    bool verified, Optional<SystemTimePoint> bannedUntil)
{
    std::string flags = std::string(role);
    if (!verified)
        flags += ", unverified";
    if (bannedUntil && *bannedUntil > SystemTimePoint::clock::now())
        flags += ", banned " + FormatBanEnd(*bannedUntil);
    return Rooftree::StringFormat("{:>6}  {:<20}  {}  <{}>  {}", id, username, displayName, email, flags);
}

// account list, lookup account: the BLOG_SEL_ADMIN_USERS rows
Rooftree::Task<void> FindAccounts(CommandHandler& handler, std::string text)
{
    std::vector<std::string> lines = co_await handler.GetSession().OnWorker([text]
    {
        // LIKE's own characters are literal
        std::string pattern;
        for (char c : text)
        {
            if (c == '%' || c == '_' || c == '\\')
                pattern += '\\';
            pattern += c;
        }
        pattern = "%" + pattern + "%";

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_USERS);
        stmt->setString(0, text);
        stmt->setString(1, pattern);
        stmt->setString(2, pattern);
        stmt->setString(3, pattern);
        stmt->setUInt32(4, ListedAccounts + 1);
        stmt->setUInt32(5, 0);
        std::vector<std::string> lines;
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
            {
                Field* fields = rows->Fetch();
                lines.push_back(FormatAccount(fields[0].GetUInt32(), fields[1].GetString(), fields[2].GetString(), fields[3].GetString(),
                    fields[4].GetString(), fields[5].GetInt64() != 0, fields[6].GetDateOrNull()));
            } while (rows->NextRow());
        }
        return lines;
    });
    if (lines.empty())
        co_return handler.SendSysMessage(text.empty() ? "There are no accounts" : "No account matches");

    bool more = lines.size() > ListedAccounts;
    lines.resize(std::min<std::size_t>(lines.size(), ListedAccounts));
    std::string out;
    for (std::string const& line : lines)
        out += line + "\n";
    out += more ? Rooftree::StringFormat("The newest {}, there are more: 'lookup account <text>' narrows it down", ListedAccounts)
        : Rooftree::StringFormat("Accounts: {}", lines.size());
    handler.SendSysMessage(out);
}

void ListAccounts(CommandHandler& handler, std::string text)
{
    handler.Spawn(FindAccounts(handler, std::move(text)));
}

bool HandleAccountList(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    ListAccounts(handler, "");
    return true;
}

bool HandleLookupAccount(CommandHandler& handler, std::string_view args)
{
    std::string text(args);
    if (text.empty() || CountCharacters(text) > 100)
        return false;

    ListAccounts(handler, text);
    return true;
}

void SetPassword(CommandHandler& handler, std::string const& account, std::string const& password)
{
    BlogSettings const& settings = sWorld->GetSettings();
    WithAccount(handler, account, [password, iterations = settings.Argon2Iterations, memoryCost = settings.Argon2MemoryCost](AccountResult& result)
    {
        // As --create-admin and the register form ask
        if (CountCharacters(password) < (result.User.IsAdmin() ? 12u : 8u))
        {
            result.Result = Outcome::PasswordTooShort;
            return;
        }
        Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash(password, iterations, memoryCost);
        if (!hash)
        {
            result.Result = Outcome::Failed;
            return;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_PASSWORD);
        stmt->setString(0, *hash);
        stmt->setUInt32(1, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
        WorkerDeleteSessions(result.User.Id);
    }, [](CommandHandler& handler, AccountResult const& result)
    {
        sWorld->ForgetUserAuth(result.User.Id);
        RT_LOG_INFO("blog.admin", "Console: changed the password of {} ({})", result.User.Username, result.User.Id);
        handler.PSendSysMessage("Changed the password of {} ({}), its sessions ended", result.User.Username, result.User.Id);
    });
}

bool HandleAccountSetPassword(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    auto check = [](CommandHandler& handler, std::string const& password, std::string const& again)
    {
        if (password != again)
            handler.SendSysMessage("The passwords differ");
        else if (password.size() > 128)
            handler.SendSysMessage("The password may have at most 128 bytes");
        else
            return true;
        return false;
    };

    if (words.size() == 3)
    {
        if (check(handler, words[1], words[2]))
            SetPassword(handler, words[0], words[1]);
        return true;
    }

    if (words.size() != 1)
        return false;

    // Not on the command line, which a terminal shows and a history keeps
    std::string account = words[0];
    bool asked = handler.Ask("New password: ", true, [account, check](CommandHandler& handler, std::string password)
    {
        handler.Ask("Again: ", true, [account, check, password = std::move(password)](CommandHandler& handler, std::string again)
        {
            if (check(handler, password, again))
                SetPassword(handler, account, password);
        });
    });
    if (!asked)
        handler.SendSysMessage("This console can't ask, give the password twice after the account");
    return true;
}

bool HandleAccountSet2fa(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.empty() || words.size() > 2 || (words.size() == 2 && words[1] != "off"))
        return false;

    bool off = words.size() == 2;
    WithAccount(handler, words[0], [off](AccountResult& result)
    {
        if (off)
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_DISABLE);
            stmt->setUInt32(0, result.User.Id);
            BlogDatabase.DirectExecute(stmt);
            return;
        }

        result.TotpSecret.resize(Rooftree::Crypto::TOTP::SecretSize);
        Rooftree::Crypto::GetRandomBytes(result.TotpSecret);
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_SET);
        stmt->setBinary(0, std::span<uint8 const>(result.TotpSecret));
        stmt->setUInt32(1, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
        // Whoever has the old secret is logged out
        WorkerDeleteSessions(result.User.Id);
    }, [off, issuer = sWorld->GetSettings().TotpIssuer](CommandHandler& handler, AccountResult const& result)
    {
        sWorld->ForgetUserAuth(result.User.Id);
        RT_LOG_INFO("blog.admin", "Console: {} the two-step login of {} ({})", off ? "turned off" : "set a new secret for", result.User.Username,
            result.User.Id);
        if (off)
            return handler.PSendSysMessage("{} ({}) logs in without a code now", result.User.Username, result.User.Id);

        // The socket client's terminal, or the server's own
        handler.PSendSysMessage("New two-step login secret for {} ({}), its sessions ended.\n\n{}", result.User.Username, result.User.Id,
            FormatTotpSetup(issuer, result.User.Username, result.TotpSecret, true));
    });
    return true;
}

// account set username / email: the new value must be free
bool SetUnique(CommandHandler& handler, std::string_view args, bool email)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.size() != 2)
        return false;

    std::string value = words[1];
    if (email ? !IsValidEmail(value) : !IsValidUsername(value))
    {
        handler.SendSysMessage(email ? "That is no email address" : "A user name has 3 to 20 letters, digits or _");
        return true;
    }

    WithAccount(handler, words[0], [value, email](AccountResult& result)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CONFLICTS);
        stmt->setString(0, email ? "" : value);
        stmt->setString(1, email ? value : "");
        stmt->setString(2, email ? "" : value);
        stmt->setString(3, email ? value : "");
        if (PreparedQueryResult conflicts = BlogDatabase.Query(stmt))
        {
            // Unless it is the account's own, in another case
            do
            {
                if (conflicts->Fetch()[email ? 1 : 0].GetInt64() && !Rooftree::StringEqualI(email ? result.User.Email : result.User.Username, value))
                {
                    result.Result = Outcome::Taken;
                    return;
                }
            } while (conflicts->NextRow());
        }

        stmt = BlogDatabase.GetPreparedStatement(email ? BLOG_UPD_USER_EMAIL : BLOG_UPD_USER_USERNAME);
        stmt->setString(0, value);
        stmt->setUInt32(1, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
    }, [value, email](CommandHandler& handler, AccountResult const& result)
    {
        // Read again by the next request
        sWorld->ForgetUserAuth(result.User.Id);
        RT_LOG_INFO("blog.admin", "Console: the {} of {} ({}) is {} now", email ? "address" : "user name", result.User.Username, result.User.Id, value);
        handler.PSendSysMessage("The {} of {} ({}) is {} now{}", email ? "address" : "user name", result.User.Username, result.User.Id, value,
            email ? ", verified" : "");
    });
    return true;
}

bool HandleAccountSetUsername(CommandHandler& handler, std::string_view args)
{
    return SetUnique(handler, args, false);
}

bool HandleAccountSetEmail(CommandHandler& handler, std::string_view args)
{
    return SetUnique(handler, args, true);
}

bool HandleAccountSetName(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.size() < 2)
        return false;

    std::string name = Join(words, 1);
    if (CountCharacters(name) > 40)
    {
        handler.SendSysMessage("A display name has at most 40 characters");
        return true;
    }

    WithAccount(handler, words[0], [name](AccountResult& result)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_DISPLAY_NAME);
        stmt->setString(0, name);
        stmt->setUInt32(1, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
    }, [name](CommandHandler& handler, AccountResult const& result)
    {
        sWorld->ForgetUserAuth(result.User.Id);
        handler.PSendSysMessage("{} ({}) is called {} now", result.User.Username, result.User.Id, name);
    });
    return true;
}

bool HandleAccountDelete(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.size() != 1)
        return false;

    WithAccount(handler, words[0], [directory = sWorld->GetSettings().UploadDirectory, driveDirectory = sWorld->GetSettings().DriveDirectory](AccountResult& result)
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_OWNED_CONTENT);
        stmt->setUInt32(0, result.User.Id);
        stmt->setUInt32(1, result.User.Id);
        if (PreparedQueryResult owned = BlogDatabase.Query(stmt))
        {
            result.Posts = owned->Fetch()[0].GetInt64();
            result.Uploads = owned->Fetch()[1].GetInt64();
        }
        if (result.Posts || result.Uploads)
        {
            result.Result = Outcome::HasContent;
            return;
        }

        // Its sessions, mailed tokens, comments and reactions go with it, its picture and drive too
        Drive::WorkerDeleteAccountDrive(result.User.Id, driveDirectory);
        stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER);
        stmt->setUInt32(0, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
        RemoveAvatarFile(directory, result.User.Avatar);
    }, [](CommandHandler& handler, AccountResult const& result)
    {
        sWorld->ForgetUserAuth(result.User.Id);
        sWorld->RequestCommentCounts();
        sWorld->AnnounceChanged("*", "comments");
        sWorld->AnnounceChanged("*", "reactions");
        RT_LOG_INFO("blog.admin", "Console: deleted {} ({})", result.User.Username, result.User.Id);
        handler.PSendSysMessage("Deleted {} ({}){}", result.User.Username, result.User.Id,
            result.User.IsAdmin() ? ", --create-admin makes a new admin" : "");
    });
    return true;
}

// ban account / unban account. end: none to lift the ban
void SetAccountBan(CommandHandler& handler, std::string const& account, Optional<SystemTimePoint> end, std::string reason)
{
    WithAccount(handler, account, [end, reason](AccountResult& result)
    {
        if (result.User.IsAdmin())
        {
            result.Result = Outcome::AdminCantBeBanned;
            return;
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_BAN);
        if (end)
        {
            stmt->setDate(0, *end);
            stmt->setString(1, reason);
        }
        else
        {
            stmt->setNull(0);
            stmt->setNull(1);
        }
        stmt->setUInt32(2, result.User.Id);
        BlogDatabase.DirectExecute(stmt);
    }, [end, reason](CommandHandler& handler, AccountResult const& result)
    {
        // As the admin pages do: the next request of a logged in user sees it
        sWorld->UpdateCachedUser(result.User.Id, [&](UserRow& cached)
        {
            cached.BannedUntil = end;
            cached.BanReason = end ? reason : "";
        });

        if (!end)
        {
            RT_LOG_INFO("blog.admin", "Console: lifted the ban of {} ({})", result.User.Username, result.User.Id);
            return handler.PSendSysMessage("{} ({}) is not banned", result.User.Username, result.User.Id);
        }

        RT_LOG_INFO("blog.admin", "Console: banned {} ({}) {}: {}", result.User.Username, result.User.Id, FormatBanEnd(*end), reason);
        handler.PSendSysMessage("Banned {} ({}) {}", result.User.Username, result.User.Id, FormatBanEnd(*end));
    });
}

// ban ip / unban ip. end: none to lift the ban
Rooftree::Task<void> SaveIpBan(CommandHandler& handler, std::string ip, Optional<SystemTimePoint> end, std::string reason)
{
    boost::system::error_code error;
    boost::asio::ip::address address = boost::asio::ip::make_address(ip, error);
    if (error)
        co_return handler.PSendSysMessage("{} is no address", ip);

    // The canonical spelling: what IsIpBanned compares against
    std::string canonical = address.to_string();
    if (!end && std::ranges::none_of(sWorld->GetIpBans(), [&](IpBan const& ban) { return ban.Ip == canonical; }))
        co_return handler.PSendSysMessage("{} is not banned", canonical);

    std::vector<IpBan> bans = co_await handler.GetSession().OnWorker([canonical, end, reason]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(end ? BLOG_REP_IP_BAN : BLOG_DEL_IP_BAN);
        stmt->setString(0, canonical);
        if (end)
        {
            stmt->setString(1, reason);
            // NULL: for good
            if (*end < PermanentBanTime())
                stmt->setDate(2, *end);
            else
                stmt->setNull(2);
        }
        BlogDatabase.DirectExecute(stmt);
        return LoadIpBansDirect();
    });
    sWorld->SetIpBans(std::move(bans));
    if (!end)
    {
        RT_LOG_INFO("blog.admin", "Console: lifted the ban of the address {}", canonical);
        co_return handler.PSendSysMessage("{} is not banned", canonical);
    }

    RT_LOG_INFO("blog.admin", "Console: banned the address {} {}: {}", canonical, FormatBanEnd(*end), reason);
    handler.PSendSysMessage("Banned {} {}: no registering or commenting from it", canonical, FormatBanEnd(*end));
}

void SetIpBan(CommandHandler& handler, std::string ip, Optional<SystemTimePoint> end, std::string reason)
{
    handler.Spawn(SaveIpBan(handler, std::move(ip), end, std::move(reason)));
}

// drive list: who has a drive, or files from when they had one
Rooftree::Task<void> ListDrives(CommandHandler& handler)
{
    BlogSettings const& settings = sWorld->GetSettings();
    std::vector<std::string> lines = co_await handler.GetSession().OnWorker([quota = settings.DriveQuota, adminQuota = settings.DriveAdminQuota]
    {
        std::vector<std::string> lines;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_DRIVE_ACCOUNTS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                std::vector<std::string> granted = SplitList(fields[3].GetString());
                bool admin = fields[2].GetString() == RoleAdmin;
                bool enabled = admin || std::ranges::find(granted, GrantDrive) != granted.end();
                uint64 size = fields[4].GetUInt64OrNull().value_or(admin ? adminQuota : quota);
                constexpr uint64 MiB = 1024 * 1024;
                lines.push_back(Rooftree::StringFormat("{:>6}  {:<20}  {} MiB of {} MiB, {} files{}", fields[0].GetUInt32(), fields[1].GetString(),
                    fields[5].GetUInt64() / MiB, size / MiB, fields[6].GetInt64(), enabled ? "" : ", off"));
            } while (rows->NextRow());
        }
        return lines;
    });
    if (lines.empty())
        co_return handler.SendSysMessage("No account has a drive");

    std::string out;
    for (std::string const& line : lines)
        out += line + "\n";
    out += Rooftree::StringFormat("Drives: {}", lines.size());
    handler.SendSysMessage(out);
}

bool HandleDriveList(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    handler.Spawn(ListDrives(handler));
    return true;
}

bool HandleBanAccount(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    Optional<SystemTimePoint> end = words.size() >= 2 ? ParseBanEnd(words[1]) : Optional<SystemTimePoint>();
    std::string reason = Join(words, 2);
    if (!end || CountCharacters(reason) > 255)
        return false;

    SetAccountBan(handler, words[0], end, reason);
    return true;
}

bool HandleBanIp(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    Optional<SystemTimePoint> end = words.size() >= 2 ? ParseBanEnd(words[1]) : Optional<SystemTimePoint>();
    std::string reason = Join(words, 2);
    if (!end || CountCharacters(reason) > 255)
        return false;

    SetIpBan(handler, words[0], end, reason);
    return true;
}

bool HandleUnbanAccount(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.size() != 1)
        return false;

    SetAccountBan(handler, words[0], {}, "");
    return true;
}

bool HandleUnbanIp(CommandHandler& handler, std::string_view args)
{
    std::vector<std::string> words = Rooftree::Server::SplitCommandArgs(args);
    if (words.size() != 1)
        return false;

    SetIpBan(handler, words[0], {}, "");
    return true;
}

Rooftree::Task<void> ListBannedAccounts(CommandHandler& handler)
{
    std::vector<std::string> lines = co_await handler.GetSession().OnWorker([]
    {
        std::vector<std::string> lines;
        if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_BANNED_USERS)))
        {
            do
            {
                Field* fields = rows->Fetch();
                lines.push_back(Rooftree::StringFormat("{:>6}  {:<20}  {}  {}", fields[0].GetUInt32(), fields[1].GetString(),
                    FormatBanEnd(fields[2].GetDate()), fields[3].GetString()));
            } while (rows->NextRow());
        }
        return lines;
    });
    if (lines.empty())
        co_return handler.SendSysMessage("No account is banned");

    std::string out;
    for (std::string const& line : lines)
        out += (out.empty() ? "" : "\n") + line;
    handler.SendSysMessage(out);
}

bool HandleBanlistAccount(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    handler.Spawn(ListBannedAccounts(handler));
    return true;
}

bool HandleBanlistIp(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    std::vector<IpBan> const& bans = sWorld->GetIpBans();
    if (bans.empty())
    {
        handler.SendSysMessage("No address is banned");
        return true;
    }

    std::string out;
    for (IpBan const& ban : bans)
        out += Rooftree::StringFormat("{}{:<39}  {}  {}", out.empty() ? "" : "\n", ban.Ip, FormatBanEnd(ban.ExpiresAt.value_or(PermanentBanTime())), ban.Reason);
    handler.SendSysMessage(out);
    return true;
}

bool HandleServerInfo(CommandHandler& handler, std::string_view args)
{
    if (!args.empty() || !ConsoleUpdateLoop)
        return false;

    uint64 uptime = uint64(std::chrono::duration_cast<Seconds>(TimePoint::clock::now() - ConsoleStartTime).count());
    Rooftree::Server::SessionMgrStats const& sessions = sWorld->GetStats();
    Rooftree::Server::UpdateLoopStats const& updates = ConsoleUpdateLoop->GetStats();

    std::string out = Rooftree::StringFormat("Up for {}. Sessions: {}; since the start {} messages, {} rejected, {} over an AntiDOS limit",
        Rooftree::FormatDuration(Seconds(uptime)), sWorld->GetSessionCount(), sessions.ProcessedMessages, sessions.RejectedMessages, sessions.AntiDosViolations);
    if (updates.Updates)
        out += Rooftree::StringFormat("\nSince the last stats log: {} updates, update time avg {} us max {} us, longest diff {} ms", updates.Updates,
            updates.TotalUpdateTime.count() / updates.Updates, updates.MaxUpdateTime.count(), updates.MaxDiff);

    // Resident pages, the second number of statm
    std::ifstream statm("/proc/self/statm");
    uint64 pages = 0;
    uint64 resident = 0;
    if (statm >> pages >> resident)
        out += Rooftree::StringFormat("\nMemory: {} MiB resident", resident * uint64(sysconf(_SC_PAGESIZE)) / (1024 * 1024));

    if (sWorld->IsRebuildRunning())
        out += "\nThe site is being rebuilt";
    if (Optional<TimePoint> stopAt = ConsoleUpdateLoop->GetScheduledStop())
        out += Rooftree::StringFormat("\nStopping in {}", Rooftree::FormatDuration(std::max(Seconds(0),
            std::chrono::duration_cast<Seconds>(*stopAt - TimePoint::clock::now()))));
    handler.SendSysMessage(out);
    return true;
}

bool HandleServerReload(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    // The way kill -HUP goes: the config at the next update, the certificate right away
    std::raise(SIGHUP);
    handler.SendSysMessage("Reloading blog.yaml and the certificate, the log says how it went");
    return true;
}

bool ScheduleStop(CommandHandler& handler, std::string_view args, int exitCode)
{
    std::string text(args);
    if (!ConsoleUpdateLoop || text.empty())
        return false;

    std::string_view what = exitCode == RestartExitCode ? "restart" : "shutdown";
    if (text == "cancel")
    {
        if (!ConsoleUpdateLoop->CancelScheduledStop())
        {
            handler.SendSysMessage("No stop is scheduled");
            return true;
        }

        RT_LOG_INFO("server.world", "Console: the scheduled stop was cancelled");
        handler.SendSysMessage("Cancelled the stop");
        return true;
    }

    Optional<Seconds> delay = ParseDelay(text);
    if (!delay)
        return false;

    ConsoleUpdateLoop->StopAfter(*delay, exitCode);
    RT_LOG_INFO("server.world", "Console: {} in {} s, exit code {}", what, delay->count(), exitCode);
    handler.PSendSysMessage("The server stops in {} (exit code {}{}). '{} cancel' keeps it running", Rooftree::FormatDuration(*delay),
        exitCode, exitCode == RestartExitCode ? ", a supervisor restarting on failure starts it again" : "",
        exitCode == RestartExitCode ? "server restart" : "server shutdown");
    return true;
}

// The profile's texts in both languages, the rest plain
constexpr std::string_view LocalizedSiteFields[] = { "name", "tagline", "bio", "quote", "about", "aboutSite" };
constexpr std::string_view PlainSiteFields[] = { "author", "avatar", "since" };

std::string_view NextWord(std::string_view& args)
{
    std::size_t start = args.find_first_not_of(' ');
    args.remove_prefix(start == std::string_view::npos ? args.size() : start);
    std::size_t end = std::min(args.find(' '), args.size());
    std::string_view word = args.substr(0, end);
    args.remove_prefix(end);
    if (!args.empty())
        args.remove_prefix(1);
    return word;
}

std::string SiteText(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    return value && value->is_string() ? std::string(value->as_string()) : "";
}

// A text of the profile on one line: the first line of a long one, with its length
std::string ShortSiteText(std::string text)
{
    std::size_t newline = text.find('\n');
    if (newline == std::string::npos && text.size() <= 80)
        return text;
    return Rooftree::StringFormat("{} ... ({} characters)", text.substr(0, std::min<std::size_t>(newline, 80)), Rooftree::Utf8Length(text));
}

// Validates, saves and applies profile like the admin's page, and says how it went
Rooftree::Task<void> StoreSiteProfile(CommandHandler& handler, boost::json::object body)
{
    boost::json::object errors;
    Optional<boost::json::object> profile = ReadSiteProfile(body, errors);
    if (!profile)
    {
        std::string out = "Nothing was saved:";
        for (auto const& [key, code] : errors)
            out += Rooftree::StringFormat("\n  {}: {}", std::string_view(key), code.is_string() ? std::string_view(code.as_string()) : "invalid");
        handler.SendSysMessage(out);
        co_return;
    }

    std::string json = boost::json::serialize(*profile);
    co_await handler.GetSession().OnWorker([json] { WorkerSaveSiteProfile(json); });
    RT_LOG_INFO("blog.admin", "Console: updated the site's profile");
    ApplySiteProfile(std::move(*profile));
    handler.SendSysMessage("Saved, the pages are rebuilt in a moment");
}

void SaveSiteProfile(CommandHandler& handler, boost::json::object const& body)
{
    handler.Spawn(StoreSiteProfile(handler, body));
}

bool HandleSiteShow(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    boost::json::object const& profile = sWorld->GetSiteProfile();
    std::string out;
    for (std::string_view key : LocalizedSiteFields)
    {
        boost::json::value const* texts = profile.if_contains(key);
        for (std::string_view lang : { "zh", "en" })
            out += Rooftree::StringFormat("{:<10} {}  {}\n", key, lang, texts && texts->is_object() ? ShortSiteText(SiteText(texts->as_object(), lang)) : "");
    }
    for (std::string_view key : PlainSiteFields)
        out += Rooftree::StringFormat("{:<10}     {}\n", key, SiteText(profile, key));
    if (boost::json::value const* socials = profile.if_contains("socials"); socials && socials->is_array())
        for (boost::json::value const& social : socials->as_array())
            if (social.is_object())
                out += Rooftree::StringFormat("{:<10}     {} {}\n", "socials", SiteText(social.as_object(), "kind"), SiteText(social.as_object(), "url"));
    out += "'site export' prints all of it as JSON";
    handler.SendSysMessage(out);
    return true;
}

bool HandleSiteExport(CommandHandler& handler, std::string_view args)
{
    if (!args.empty())
        return false;

    handler.SendSysMessage(boost::json::serialize(sWorld->GetSiteProfile()));
    return true;
}

bool HandleSiteImport(CommandHandler& handler, std::string_view args)
{
    if (args.empty())
        return false;

    boost::system::error_code error;
    boost::json::value value = boost::json::parse(args, error);
    if (error || !value.is_object())
    {
        handler.SendSysMessage("That is not a JSON object, 'site export' shows one");
        return true;
    }

    SaveSiteProfile(handler, value.as_object());
    return true;
}

// site set <field> [zh|en] <text>: one field, the rest as it is; no text empties it
bool HandleSiteSet(CommandHandler& handler, std::string_view args)
{
    std::string key(NextWord(args));
    bool localized = std::ranges::find(LocalizedSiteFields, key) != std::end(LocalizedSiteFields);
    if (!localized && std::ranges::find(PlainSiteFields, key) == std::end(PlainSiteFields))
        return false;

    boost::json::object profile = sWorld->GetSiteProfile();
    if (localized)
    {
        std::string lang(NextWord(args));
        if (lang != "zh" && lang != "en")
            return false;

        boost::json::value& texts = profile[key];
        if (!texts.is_object())
            texts = boost::json::object();
        texts.as_object()[lang] = args;
    }
    else
        profile[key] = args;

    SaveSiteProfile(handler, profile);
    return true;
}

// On: writers' publishing asks the admin first (post list review, post publish, post decline)
bool HandleSiteReview(CommandHandler& handler, std::string_view args)
{
    Optional<bool> enabled = args == "on" ? Optional<bool>(true) : args == "off" ? Optional<bool>(false) : Optional<bool>();
    if (!enabled)
        return false;
    boost::json::object profile = sWorld->GetSiteProfile();
    profile["reviewWriters"] = *enabled;
    SaveSiteProfile(handler, profile);
    return true;
}

bool HandleServerShutdown(CommandHandler& handler, std::string_view args)
{
    return ScheduleStop(handler, args, 0);
}

bool HandleServerRestart(CommandHandler& handler, std::string_view args)
{
    return ScheduleStop(handler, args, RestartExitCode);
}
}

void SetConsoleUpdateLoop(Rooftree::Server::UpdateLoop* loop)
{
    ConsoleUpdateLoop = loop;
    ConsoleStartTime = TimePoint::clock::now();
}

Rooftree::Server::CommandTable const& GetConsoleCommands()
{
    static Rooftree::Server::CommandTable const commands([]
    {
    std::vector<Rooftree::Server::Command> list =
    {
        GetConsoleApiCommand(),
        {
            .Name = "account",
            .Help = "Accounts, by user name or email address",
            .Subcommands =
            {
                {
                    .Name = "list",
                    .Help = "Syntax: account list\nThe newest accounts.",
                    .Handler = &HandleAccountList,
                },
                {
                    .Name = "set",
                    .Help = "Change an account",
                    .Subcommands =
                    {
                        {
                            .Name = "password",
                            .Help = "Syntax: account set password <account>\n"
                                "Asks for the password twice, not echoed (the admin's at least 12 characters, others 8), and ends the account's sessions.\n"
                                "'account set password <account> <password> <password>' takes it from the line instead.",
                            .Handler = &HandleAccountSetPassword,
                        },
                        {
                            .Name = "2fa",
                            .Help = "Syntax: account set 2fa <account> [off]\n"
                                "Makes a new two-step login secret, shown as a QR code, and ends the account's sessions.\n"
                                "off: logs in without a code.",
                            .Handler = &HandleAccountSet2fa,
                        },
                        {
                            .Name = "username",
                            .Help = "Syntax: account set username <account> <new name>\n3 to 20 letters, digits or _, not another account's.",
                            .Handler = &HandleAccountSetUsername,
                        },
                        {
                            .Name = "email",
                            .Help = "Syntax: account set email <account> <address>\nNot another account's; counts as verified.",
                            .Handler = &HandleAccountSetEmail,
                        },
                        {
                            .Name = "name",
                            .Help = "Syntax: account set name <account> <display name>\nAt most 40 characters, spaces allowed.",
                            .Handler = &HandleAccountSetName,
                        },
                    },
                },
                {
                    .Name = "delete",
                    .Help = "Syntax: account delete <account>\n"
                        "Deletes the account with its comments and reactions. Not while it has posts or uploads.",
                    .Handler = &HandleAccountDelete,
                },
            },
        },
        {
            .Name = "lookup",
            .Help = "Find things",
            .Subcommands =
            {
                {
                    .Name = "account",
                    .Help = "Syntax: lookup account <text>\nThe accounts whose user name, address or display name contains text.",
                    .Handler = &HandleLookupAccount,
                },
            },
        },
        {
            .Name = "drive",
            .Help = "Accounts' drives",
            .Subcommands =
            {
                {
                    .Name = "list",
                    .Help = "Syntax: drive list\nThe accounts with a drive (the admin gives one on the account's page): used, size, files.",
                    .Handler = &HandleDriveList,
                },
            },
        },
        {
            .Name = "ban",
            .Help = "Ban an account or an address",
            .Subcommands =
            {
                {
                    .Name = "account",
                    .Help = "Syntax: ban account <account> <time|perm> [reason]\n"
                        "Time like 30m, 12h, 7d, 1d12h; perm for good. The user sees the reason. Not the admin.",
                    .Handler = &HandleBanAccount,
                },
                {
                    .Name = "ip",
                    .Help = "Syntax: ban ip <address> <time|perm> [reason]\nNo registering or commenting from the address.",
                    .Handler = &HandleBanIp,
                },
            },
        },
        {
            .Name = "unban",
            .Help = "Lift a ban",
            .Subcommands =
            {
                { .Name = "account", .Help = "Syntax: unban account <account>", .Handler = &HandleUnbanAccount },
                { .Name = "ip", .Help = "Syntax: unban ip <address>", .Handler = &HandleUnbanIp },
            },
        },
        {
            .Name = "banlist",
            .Help = "What is banned",
            .Subcommands =
            {
                { .Name = "account", .Help = "Syntax: banlist account", .Handler = &HandleBanlistAccount },
                { .Name = "ip", .Help = "Syntax: banlist ip", .Handler = &HandleBanlistIp },
            },
        },
        {
            .Name = "site",
            .Help = "The site's profile: the home page's introduction, the about pages, the links",
            .Subcommands =
            {
                { .Name = "show", .Help = "Syntax: site show\nEvery field, long texts shortened.", .Handler = &HandleSiteShow },
                { .Name = "export", .Help = "Syntax: site export\nThe profile as JSON on one line, for 'site import'.", .Handler = &HandleSiteExport },
                { .Name = "review", .Help = "Syntax: site review on|off\nOn: writers' publishing asks the admin first (post list review).", .Handler = &HandleSiteReview },
                {
                    .Name = "import",
                    .Help = "Syntax: site import <json>\n"
                        "Replaces the whole profile, checked like the admin's page: what 'site export' prints, on one line.",
                    .Handler = &HandleSiteImport,
                },
                {
                    .Name = "set",
                    .Help = "Syntax: site set <field> [zh|en] <text>\n"
                        "One field, the others as they are; no text empties it.\n"
                        "name, tagline, bio, quote, about, aboutSite take a language; author, avatar, since (YYYY-MM-DD) don't.\n"
                        "Texts of more than one line and the links: 'site import'.",
                    .Handler = &HandleSiteSet,
                },
            },
        },
        {
            .Name = "server",
            .Help = "The server itself",
            .Subcommands =
            {
                { .Name = "info", .Help = "Syntax: server info\nUptime, sessions, update times, memory.", .Handler = &HandleServerInfo },
                { .Name = "reload", .Help = "Syntax: server reload\nWhat kill -HUP does: blog.yaml and the certificate.", .Handler = &HandleServerReload },
                {
                    .Name = "shutdown",
                    .Help = "Syntax: server shutdown <time>|cancel\n"
                        "Stops the server after time (seconds, or like 5m), exit code 0. Docker's restart policy or systemd's decides what comes next.",
                    .Handler = &HandleServerShutdown,
                },
                {
                    .Name = "restart",
                    .Help = "Syntax: server restart <time>|cancel\n"
                        "As shutdown, with exit code 2: systemd's Restart=on-failure starts it again.",
                    .Handler = &HandleServerRestart,
                },
            },
        },
    };

    // The admin's pages (ConsoleAdmin.h): their own commands, and more of account's
    auto account = std::ranges::find(list, std::string_view("account"), &Rooftree::Server::Command::Name);
    for (Rooftree::Server::Command& command : GetConsoleAccountAdminCommands())
        account->Subcommands.push_back(std::move(command));
    for (Rooftree::Server::Command& command : GetConsoleAdminCommands())
        list.push_back(std::move(command));
    return list;
    }());
    return commands;
}
}
