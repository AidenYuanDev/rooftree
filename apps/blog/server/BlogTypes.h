/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Rows and values the blog passes around, and how they look in JSON. Times are UTC everywhere: in the
 * database (DATETIME written with setDate) and in JSON (ISO 8601 with Z); the frontend shows them in
 * the reader's time zone.
 */

#ifndef BLOG_TYPES_H
#define BLOG_TYPES_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <algorithm>
#include <boost/json/object.hpp>
#include <string>
#include <string_view>
#include <vector>

class Field;

namespace Blog
{
inline constexpr std::string_view RoleAdmin = "admin";
inline constexpr std::string_view RoleModerator = "moderator";
inline constexpr std::string_view RoleUser = "user";

// What the admin can take from an account, one by one (user.denied)
inline constexpr std::string_view DenyComment = "comment";
inline constexpr std::string_view DenyReact = "react";
inline constexpr std::string_view DenyAvatar = "avatar";
inline constexpr std::string_view DenyFriend = "friend";
inline constexpr std::string_view DenyChat = "chat";
inline constexpr std::string_view DeniableActions[] = { DenyComment, DenyReact, DenyAvatar, DenyFriend, DenyChat };

// What the admin can give an account on top of its role (user.granted); the admin has all of them
inline constexpr std::string_view GrantDrive = "drive";
// Writes posts of its own (and edits the ones it is invited to, which needs nothing)
inline constexpr std::string_view GrantWrite = "write";
inline constexpr std::string_view GrantableActions[] = { GrantDrive, GrantWrite };

// What an editor of a post may do on top of editing its text (post_editor.rights): publish it (and
// take it back, or schedule it), change its settings (address, cover, tags, languages, comments),
// invite and remove editors. Its owner and the admin may do all of it, and delete it
inline constexpr std::string_view PostRightPublish = "publish";
inline constexpr std::string_view PostRightSettings = "settings";
inline constexpr std::string_view PostRightEditors = "editors";
inline constexpr std::string_view PostRights[] = { PostRightPublish, PostRightSettings, PostRightEditors };

inline bool IsValidLang(std::string_view lang) { return lang == "zh" || lang == "en"; }

// banned_until of a ban for good, also "after everything" for cursors: 2200-01-01
SystemTimePoint PermanentBanTime();

// What the site has published, for its page (SiteHandlers.cpp, ReadCache.h)
struct SiteCounts
{
    int64 Posts = 0;
    int64 Notes = 0;
    int64 Thinking = 0;
    int64 Words = 0;
};

struct UserRow
{
    uint32 Id = 0;
    std::string Username;
    std::string DisplayName;
    std::string Email;
    bool EmailVerified = false;
    std::string PasswordHash;
    bool HasPassword = false;       // not when made by a GitHub or Google login, until one is set
    std::string Role;
    std::string Locale;
    Optional<std::vector<uint8>> TotpSecret;
    Optional<int64> TotpLastStep;
    Optional<SystemTimePoint> BannedUntil;
    std::string BanReason;
    std::string Avatar;             // its file in Blog.Uploads.Directory, empty: its initials
    std::vector<std::string> Denied;    // DeniableActions the admin took from it
    bool Deleted = false;           // deleted by the admin, kept for its comments
    std::vector<std::string> Granted;   // GrantableActions the admin gave it
    Optional<uint64> DriveQuota;    // bytes, none: Blog.Drive.Quota (AdminQuota)
    Optional<uint32> DriveTrashDays;    // none: Blog.Drive.TrashDays
    bool ReplyMails = true;             // a mail when someone replies to its comments
    bool EditsPosts = false;            // invited to edit a post (post_editor)

    // The BLOG_USER_COLUMNS of BlogDatabase.cpp, starting at fields[0]
    static UserRow FromFields(Field const* fields);

    bool IsAdmin() const { return Role == RoleAdmin; }
    // Hides comments, bans users: the admin and moderators
    bool CanModerate() const { return Role == RoleAdmin || Role == RoleModerator; }
    bool IsDenied(std::string_view action) const { return std::ranges::find(Denied, action) != Denied.end(); }
    bool IsGranted(std::string_view action) const { return IsAdmin() || std::ranges::find(Granted, action) != Granted.end(); }
    bool IsBanned(SystemTimePoint now = SystemTimePoint::clock::now()) const { return BannedUntil && *BannedUntil > now; }
};

// A logged in session, cached on the main thread (BlogWorld::AuthCache)
struct AuthInfo
{
    std::string SessionHash;
    std::string CsrfToken;
    SystemTimePoint ExpiresAt;
    SystemTimePoint LastSeenAt;
    UserRow User;           // without the password hash
};


// "2026-10-20T08:30:00Z"
std::string FormatTime(SystemTimePoint time);
boost::json::value FormatTime(Optional<SystemTimePoint> time);

// "a,b" -> {"a", "b"}, "" -> {}
std::vector<std::string> SplitList(std::string_view list);
// {"a", "b"} -> "a,b", what a SET column takes
std::string JoinList(std::vector<std::string> const& values);
}

#endif // BLOG_TYPES_H
