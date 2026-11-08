/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "AdminUsers.h"
#include "ApiCall.h"
#include "BlogWorld.h"
#include "Field.h"
#include <boost/json/array.hpp>

namespace Blog
{
AdminUserRow AdminUserRow::FromFields(Field const* fields)
{
    AdminUserRow user;
    user.Id = fields[0].GetUInt32();
    user.Username = fields[1].GetString();
    user.DisplayName = fields[2].GetString();
    user.Email = fields[3].GetString();
    user.Role = fields[4].GetString();
    user.EmailVerified = fields[5].GetInt64() != 0;
    user.BannedUntil = fields[6].GetDateOrNull();
    user.BanReason = fields[7].GetStringOrNull().value_or("");
    user.CreatedAt = fields[8].GetDate();
    user.CommentCount = fields[9].GetInt64();
    user.Avatar = fields[10].GetStringOrNull().value_or("");
    user.Denied = SplitList(fields[11].GetString());
    user.Deleted = fields[12].GetInt64() != 0;
    user.LastSeenAt = fields[13].GetDateOrNull();
    user.OnlineSeconds = fields[14].GetInt64();
    user.TotpEnabled = fields[15].GetInt64() != 0;
    user.Granted = SplitList(fields[16].GetString());
    user.DriveQuota = fields[17].GetUInt64OrNull();
    user.DriveUsed = fields[18].GetUInt64();
    user.DriveTrashDays = fields[19].GetUInt32OrNull();
    return user;
}

flatbuffers::Offset<Admin::User> AdminUserRow::BuildTable(flatbuffers::FlatBufferBuilder& builder, bool withEmail, Presence::OnlineUser const* online,
    Details const* details) const
{
    bool banned = BannedUntil && *BannedUntil > SystemTimePoint::clock::now();
    auto username = builder.CreateString(Username);
    auto displayName = builder.CreateString(DisplayName);
    auto role = builder.CreateString(Role);
    auto bannedUntil = banned ? TimeString(builder, BannedUntil) : 0;
    auto banReason = banned ? builder.CreateString(BanReason) : 0;
    auto createdAt = TimeString(builder, CreatedAt);
    auto avatar = Avatar.empty() ? 0 : builder.CreateString("/uploads/" + Avatar);
    auto denied = StringList(builder, Denied);
    // Online now, as far as the admin is concerned
    auto lastSeenAt = TimeString(builder, online ? Optional<SystemTimePoint>(SystemTimePoint::clock::now()) : LastSeenAt);

    flatbuffers::Offset<flatbuffers::String> email;
    flatbuffers::Offset<Admin::UserDrive> drive;
    if (withEmail)
    {
        email = builder.CreateString(Email);
        // How big its drive is, how long its trash keeps what was deleted
        BlogSettings const& settings = sWorld->GetSettings();
        drive = Admin::CreateUserDrive(builder, Role == RoleAdmin || std::ranges::find(Granted, GrantDrive) != Granted.end(),
            DriveQuota ? flatbuffers::Optional<uint64>(*DriveQuota) : flatbuffers::nullopt,
            Role == RoleAdmin ? settings.DriveAdminQuota : settings.DriveQuota, DriveUsed,
            DriveTrashDays ? flatbuffers::Optional<uint32>(*DriveTrashDays) : flatbuffers::nullopt, settings.DriveTrashDays);
    }

    flatbuffers::Offset<Admin::UserOnline> onlineTable;
    if (online)
    {
        std::vector<std::string> pages(online->Pages.begin(), online->Pages.end());
        onlineTable = Admin::CreateUserOnline(builder, TimeString(builder, online->Since), StringList(builder, pages), uint32(online->Connections));
    }

    Admin::UserBuilder user(builder);
    user.add_id(Id);
    user.add_username(username);
    user.add_displayName(displayName);
    user.add_role(role);
    user.add_emailVerified(EmailVerified);
    user.add_bannedUntil(bannedUntil);
    user.add_banReason(banReason);
    user.add_createdAt(createdAt);
    user.add_commentCount(uint32(CommentCount));
    user.add_avatar(avatar);
    user.add_denied(denied);
    user.add_deleted(Deleted);
    user.add_totpEnabled(TotpEnabled);
    user.add_lastSeenAt(lastSeenAt);
    // With what the server counted and didn't write yet
    user.add_onlineSeconds(uint64(OnlineSeconds + (online ? online->Unsaved.count() : 0)));
    user.add_email(email);
    user.add_drive(drive);
    user.add_online(onlineTable);
    user.add_write(Role == RoleAdmin || std::ranges::find(Granted, GrantWrite) != Granted.end());
    if (details)
    {
        user.add_reactionCount(details->ReactionCount);
        user.add_locale(details->Locale);
        user.add_hasPassword(details->HasPassword);
        user.add_identities(details->Identities);
        user.add_sessions(details->Sessions);
    }
    return user.Finish();
}
}
