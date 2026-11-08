/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * A user as the admin's pages show it (AdminHandlers.cpp lists them, UserAdminHandlers.cpp shows and
 * changes one): the account, its standing (role, bans, what it may not do) and when it was online.
 */

#ifndef BLOG_ADMIN_USERS_H
#define BLOG_ADMIN_USERS_H

#include "Admin_generated.h"
#include "BlogTypes.h"
#include "Presence.h"

namespace Blog
{
struct AdminUserRow
{
    uint32 Id = 0;
    std::string Username;
    std::string DisplayName;
    std::string Email;
    std::string Role;
    bool EmailVerified = false;
    Optional<SystemTimePoint> BannedUntil;
    std::string BanReason;
    SystemTimePoint CreatedAt;
    int64 CommentCount = 0;
    std::string Avatar;
    std::vector<std::string> Denied;
    bool Deleted = false;
    Optional<SystemTimePoint> LastSeenAt;
    int64 OnlineSeconds = 0;
    bool TotpEnabled = false;
    std::vector<std::string> Granted;
    Optional<uint64> DriveQuota;    // none: the default
    uint64 DriveUsed = 0;
    Optional<uint32> DriveTrashDays;    // none: the default

    // The BLOG_ADMIN_USER_COLUMNS of BlogDatabase.cpp, starting at fields[0]
    static AdminUserRow FromFields(Field const* fields);

    // What only UsersGet's has (UserAdminHandlers.cpp), made before the user's table
    struct Details
    {
        uint32 ReactionCount = 0;
        flatbuffers::Offset<flatbuffers::String> Locale;
        bool HasPassword = false;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Admin::UserIdentity>>> Identities;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Admin::UserSession>>> Sessions;
    };

    // Its Admin::User. withEmail: for the admin, not moderators. online: now, null when not
    flatbuffers::Offset<Admin::User> BuildTable(flatbuffers::FlatBufferBuilder& builder, bool withEmail, Presence::OnlineUser const* online,
        Details const* details = nullptr) const;
};
}

#endif // BLOG_ADMIN_USERS_H
