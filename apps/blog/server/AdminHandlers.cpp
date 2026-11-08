/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The admin's tools besides posts: the list of users and their bans (moderators' too: they ban users,
 * not each other), IP bans, image uploads. Managing the accounts themselves is UserAdminHandlers.cpp.
 *
 * A ban is banned_until on the account (a date, or 2200-01-01 for good) with a reason the user is
 * shown; the auth cache is updated at once, so the next request of a logged in user already sees it.
 * IP bans are for waves of throwaway accounts: they stop registering and commenting from an address.
 *
 * Uploads are checked by their first bytes, not by the name or the Content-Type the browser sent,
 * and stored under a random name: nothing a user chose ends up in a path.
 */

#include "Accounts.h"
#include "AdminUsers.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Drive.h"
#include "PostAccess.h"
#include "Field.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringConvert.h"
#include "Validation.h"
#include <boost/json/array.hpp>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
constexpr uint32 UsersPerPage = 50;
std::vector<IpBan> WorkerLoadIpBans()
{
    std::vector<IpBan> bans;
    if (PreparedQueryResult rows = BlogDatabase.Query(BlogDatabase.GetPreparedStatement(BLOG_SEL_IP_BANS)))
    {
        do
        {
            Field* fields = rows->Fetch();
            bans.push_back({ .Ip = fields[0].GetString(), .Reason = fields[1].GetString(), .CreatedAt = fields[2].GetDate(),
                .ExpiresAt = fields[3].GetDateOrNull() });
        } while (rows->NextRow());
    }
    return bans;
}

flatbuffers::Offset<Admin::IpBan> IpBanTable(flatbuffers::FlatBufferBuilder& builder, IpBan const& ban)
{
    return Admin::CreateIpBan(builder, builder.CreateString(ban.Ip), builder.CreateString(ban.Reason), TimeString(builder, ban.CreatedAt),
        TimeString(builder, ban.ExpiresAt));
}

// Days from now, or for good without days
Optional<SystemTimePoint> ReadBanEnd(TableFields& fields, flatbuffers::Optional<uint32> days)
{
    if (!days)
        return {};

    if (*days < 1 || *days > 3650)
    {
        fields.AddError("days", "invalid");
        return {};
    }
    return SystemTimePoint::clock::now() + std::chrono::days(*days);
}

struct ImageType
{
    std::string_view Mime;
    std::string_view Extension;
};

// By content: what the file is, whatever it is called
Optional<ImageType> DetectImage(std::string_view data)
{
    auto startsWith = [&](std::string_view magic, std::size_t offset = 0)
    {
        return data.size() >= offset + magic.size() && data.substr(offset, magic.size()) == magic;
    };

    if (startsWith("\xFF\xD8\xFF"))
        return ImageType{ "image/jpeg", "jpg" };
    if (startsWith("\x89PNG\r\n\x1A\n"))
        return ImageType{ "image/png", "png" };
    if (startsWith("GIF87a") || startsWith("GIF89a"))
        return ImageType{ "image/gif", "gif" };
    if (startsWith("RIFF") && startsWith("WEBP", 8))
        return ImageType{ "image/webp", "webp" };
    return {};
}
}

std::vector<IpBan> LoadIpBansDirect()
{
    return WorkerLoadIpBans();
}

Rooftree::Task<void> AdminListUsers(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Admin::UsersListRequest const& in = request->Get<Admin::UsersListRequest>();
    auto text = [](flatbuffers::String const* value) { return value ? value->str() : std::string(); };
    std::string query = text(in.q());
    std::string role = text(in.role());
    std::string status = text(in.status());
    bool byLastSeen = text(in.sort()) == "lastSeen";
    if (CountCharacters(query) > 100)
        co_return RespondValidation(request, { { "q", "too_long" } });
    if (!role.empty() && role != RoleAdmin && role != RoleModerator && role != RoleUser)
        co_return RespondValidation(request, { { "role", "invalid" } });
    if (!status.empty() && status != "online" && status != "banned" && status != "deleted")
        co_return RespondValidation(request, { { "status", "invalid" } });

    // Moderators see who someone is, not their address
    bool withEmail = auth->User.IsAdmin();
    // The ones online, by id; "0" for nobody
    std::string onlineIds;
    if (status == "online")
    {
        for (auto const& [userId, online] : sWorld->GetPresence().GetOnlineUsers())
            onlineIds += (onlineIds.empty() ? "" : ",") + std::to_string(userId);
        if (onlineIds.empty())
            onlineIds = "0";
    }

    uint32 page = std::max(1u, in.page().value_or(1));
    std::vector<AdminUserRow> rows = co_await session.OnWorker([=]
    {
        // LIKE's own characters in what the admin typed are literal
        std::string pattern;
        for (char c : query)
        {
            if (c == '%' || c == '_' || c == '\\')
                pattern += '\\';
            pattern += c;
        }
        pattern = "%" + pattern + "%";

        std::vector<AdminUserRow> users;
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_USERS);
        stmt->setString(0, query);
        stmt->setString(1, pattern);
        stmt->setString(2, pattern);
        stmt->setUInt8(3, withEmail ? 1 : 0);
        stmt->setString(4, pattern);
        stmt->setString(5, role);
        stmt->setString(6, role);
        stmt->setString(7, onlineIds);
        stmt->setString(8, onlineIds);
        stmt->setUInt8(9, status == "banned" ? 1 : 0);
        stmt->setUInt8(10, status == "deleted" ? 1 : 0);
        stmt->setUInt8(11, byLastSeen ? 1 : 0);
        stmt->setUInt32(12, UsersPerPage + 1);
        stmt->setUInt32(13, (page - 1) * UsersPerPage);
        if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
        {
            do
                users.push_back(AdminUserRow::FromFields(rows->Fetch()));
            while (rows->NextRow());
        }
        return users;
    });
    bool hasMore = rows.size() > UsersPerPage;
    if (hasMore)
        rows.resize(UsersPerPage);

    // Who is online now, from the main thread's Presence
    std::map<uint32, Presence::OnlineUser> online = sWorld->GetPresence().GetOnlineUsers();
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::User>> users;
    for (AdminUserRow const& row : rows)
    {
        auto itr = online.find(row.Id);
        users.push_back(row.BuildTable(builder, withEmail, itr != online.end() ? &itr->second : nullptr));
    }
    builder.Finish(Admin::CreateUsersList(builder, builder.CreateVector(users), hasMore));
    request->Respond(builder);
}

Rooftree::Task<void> AdminBanUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Admin::UsersBanRequest const& in = request->Get<Admin::UsersBanRequest>();
    if (!in.id())
        co_return RespondNotFound(request);

    uint32 id = *in.id();
    TableFields fields(request->GetErrors());
    std::string reason = fields.Text("reason", in.reason(), 0, 255, false);
    Optional<SystemTimePoint> until = ReadBanEnd(fields, in.days());
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        Optional<UserRow> User;
        std::string_view Error;
    };

    SystemTimePoint end = until.value_or(PermanentBanTime());
    bool byAdmin = auth->User.IsAdmin();
    auto by = auth->User.Username;
    Result result = co_await session.OnWorker([id, reason, end, byAdmin]
    {
        Result result;
        result.User = FindUserById(id);
        if (!result.User || result.User->Deleted)
        {
            result.User.reset();
            return result;
        }
        // The admin can't be banned, moderators only by the admin
        if (result.User->IsAdmin())
            result.Error = "cannot_ban_admin";
        else if (!byAdmin && result.User->Role != RoleUser)
            result.Error = "cannot_ban_moderator";
        if (!result.Error.empty())
            return result;

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_BAN);
        stmt->setDate(0, end);
        stmt->setString(1, reason);
        stmt->setUInt32(2, id);
        BlogDatabase.DirectExecute(stmt);
        result.User->BannedUntil = end;
        result.User->BanReason = reason;
        return result;
    });
    if (!result.User)
        co_return RespondNotFound(request);
    if (!result.Error.empty())
        co_return request->RespondError(http::status::forbidden, result.Error);

    UserRow const& user = *result.User;
    RT_LOG_INFO("blog.admin", "{} banned {} ({}) until {}: {}", by, user.Username, user.Id, FormatTime(*user.BannedUntil), user.BanReason);
    sWorld->UpdateCachedUser(user.Id, [&](UserRow& cached)
    {
        cached.BannedUntil = user.BannedUntil;
        cached.BanReason = user.BanReason;
    });
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateUsersBanned(builder, true, TimeString(builder, user.BannedUntil)));
    request->Respond(builder);
}

Rooftree::Task<void> AdminUnbanUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    flatbuffers::Optional<uint32> idParam = request->Get<Admin::IdRequest>().id();
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    bool byAdmin = auth->User.IsAdmin();
    auto by = auth->User.Username;
    std::string_view error = co_await session.OnWorker([id, byAdmin]() -> std::string_view
    {
        Optional<UserRow> user = FindUserById(id);
        if (!user)
            return "not_found";
        if (!byAdmin && user->Role != RoleUser)
            return "cannot_ban_moderator";

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_BAN);
        stmt->setNull(0);
        stmt->setNull(1);
        stmt->setUInt32(2, id);
        BlogDatabase.DirectExecute(stmt);
        return {};
    });
    if (error == "not_found")
        co_return RespondNotFound(request);
    if (!error.empty())
        co_return request->RespondError(http::status::forbidden, error);

    RT_LOG_INFO("blog.admin", "{} lifted the ban of user {}", by, id);
    sWorld->UpdateCachedUser(id, [](UserRow& user)
    {
        user.BannedUntil.reset();
        user.BanReason.clear();
    });
    RespondOk(request);
}

Rooftree::Task<void> AdminListIpBans(Session& session, ApiRequest request)
{
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::IpBan>> bans;
    for (IpBan const& ban : sWorld->GetIpBans())
        bans.push_back(IpBanTable(builder, ban));
    builder.Finish(Admin::CreateIpBansList(builder, builder.CreateVector(bans)));
    request->Respond(builder);
    co_return;
}

Rooftree::Task<void> AdminAddIpBan(Session& session, ApiRequest request)
{
    Admin::IpBansAddRequest const& in = request->Get<Admin::IpBansAddRequest>();
    TableFields fields(request->GetErrors());
    std::string ip = fields.Text("ip", in.ip(), 1, 45);
    std::string reason = fields.Text("reason", in.reason(), 0, 255, false);
    Optional<SystemTimePoint> until = ReadBanEnd(fields, in.days());
    boost::system::error_code error;
    boost::asio::ip::address address = boost::asio::ip::make_address(ip, error);
    if (!ip.empty() && error)
        fields.AddError("ip", "invalid");
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    // The canonical spelling: what IsIpBanned compares against
    std::string canonical = address.to_string();
    std::vector<IpBan> bans = co_await session.OnWorker([canonical, reason, until]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_REP_IP_BAN);
        stmt->setString(0, canonical);
        stmt->setString(1, reason);
        if (until)
            stmt->setDate(2, *until);
        else
            stmt->setNull(2);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadIpBans();
    });
    RT_LOG_INFO("blog.admin", "Banned the address {}", canonical);
    sWorld->SetIpBans(std::move(bans));
    for (IpBan const& ban : sWorld->GetIpBans())
        if (ban.Ip == canonical)
        {
            flatbuffers::FlatBufferBuilder builder;
            builder.Finish(IpBanTable(builder, ban));
            co_return request->Respond(builder, http::status::created);
        }
    RespondInternalError(request);
}

Rooftree::Task<void> AdminDeleteIpBan(Session& session, ApiRequest request)
{
    flatbuffers::String const* ipValue = request->Get<Admin::IpBanRequest>().ip();
    std::string ip = ipValue ? ipValue->str() : std::string();
    std::vector<IpBan> bans = co_await session.OnWorker([ip]
    {
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_IP_BAN);
        stmt->setString(0, ip);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadIpBans();
    });
    sWorld->SetIpBans(std::move(bans));
    RespondOk(request);
}

namespace
{
// An image for posts as the request has it, checked: its type by its bytes, its size and placeholder
struct ImageUpload
{
    std::string Name;       // random, with its type's extension
    std::string Mime;
    std::string Data;
    Optional<uint16> Width, Height;
    std::string Thumbhash;
};

// None (answered) when it isn't one
Optional<ImageUpload> ReadImageUpload(ApiRequest const& request, Admin::UploadRequest const& in)
{
    std::string_view file = in.data() ? std::string_view(reinterpret_cast<char const*>(in.data()->data()), in.data()->size()) : std::string_view();
    if (file.empty())
    {
        RespondValidation(request, { { "file", "required" } });
        return {};
    }

    if (file.size() > sWorld->GetSettings().MaxUploadSize)
    {
        RespondValidation(request, { { "file", "too_long" } });
        return {};
    }

    Optional<ImageType> type = DetectImage(file);
    if (!type)
    {
        RespondValidation(request, { { "file", "unsupported_type" } });
        return {};
    }

    // The browser sends the image's size and blurred placeholder along (thumbhash, base64)
    ImageUpload upload;
    upload.Width = in.width() ? Optional<uint16>(*in.width()) : Optional<uint16>();
    upload.Height = in.height() ? Optional<uint16>(*in.height()) : Optional<uint16>();
    upload.Thumbhash = in.thumbhash() ? in.thumbhash()->str() : std::string();
    bool validHash = upload.Thumbhash.size() <= 64 && std::ranges::all_of(upload.Thumbhash, [](char c)
    {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/' || c == '=';
    });
    if (!validHash || (upload.Width.has_value() != upload.Height.has_value()) || (upload.Width && (!*upload.Width || !*upload.Height)))
    {
        RespondValidation(request, { { "thumbhash", "invalid" } });
        return {};
    }

    upload.Name = Rooftree::Server::GenerateToken(16) + "." + std::string(type->Extension);
    upload.Mime = type->Mime;
    // The file points into the request, the worker gets its own copy
    upload.Data = file;
    return upload;
}

// Worker: the file under directory and its row, ownerId's (in its drive's space when in a post)
bool WorkerStoreUpload(ImageUpload const& upload, std::string const& directory, uint32 ownerId, Optional<uint32> postId, Optional<uint32> by)
{
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    {
        std::ofstream out(directory + "/" + upload.Name, std::ios::binary);
        out.write(upload.Data.data(), std::streamsize(upload.Data.size()));
        if (!out)
            return false;
    }

    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_UPLOAD);
    stmt->setUInt32(0, ownerId);
    stmt->setString(1, upload.Name);
    stmt->setString(2, upload.Mime);
    stmt->setUInt32(3, uint32(upload.Data.size()));
    if (upload.Width && !upload.Thumbhash.empty())
        stmt->setString(4, upload.Thumbhash);
    else
        stmt->setNull(4);
    if (upload.Width)
    {
        stmt->setUInt16(5, *upload.Width);
        stmt->setUInt16(6, *upload.Height);
    }
    else
    {
        stmt->setNull(5);
        stmt->setNull(6);
    }
    if (postId)
        stmt->setUInt32(7, *postId);
    else
        stmt->setNull(7);
    if (by)
        stmt->setUInt32(8, *by);
    else
        stmt->setNull(8);
    BlogDatabase.DirectExecute(stmt);
    return true;
}

void RespondUploaded(ApiRequest const& request, ImageUpload const& upload)
{
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateUploadedDirect(builder, ("/uploads/" + upload.Name).c_str(), upload.Mime.c_str(), upload.Data.size()));
    request->Respond(builder, http::status::created);
}
}

Rooftree::Task<void> AdminUpload(Session& session, ApiRequest request)
{
    Optional<ImageUpload> upload = ReadImageUpload(request, request->Get<Admin::UploadRequest>());
    if (!upload)
        co_return;

    bool written = co_await session.OnWorker([upload = *upload, directory = sWorld->GetSettings().UploadDirectory, userId = request->GetAuth().User.Id]
    {
        return WorkerStoreUpload(upload, directory, userId, {}, {});
    });
    if (!written)
    {
        RT_LOG_ERROR("blog.admin", "Could not write the upload {} into Blog.Uploads.Directory", upload->Name);
        co_return RespondInternalError(request);
    }

    RT_LOG_INFO("blog.admin", "Uploaded {} ({}, {} bytes)", upload->Name, upload->Mime, upload->Data.size());
    RespondUploaded(request, *upload);
}

// An image an editor puts in a post: its owner's, in the space of its owner's drive, which needs one
// with room for it
Rooftree::Task<void> AdminPostUpload(Session& session, ApiRequest request)
{
    Admin::UploadRequest const& in = request->Get<Admin::UploadRequest>();
    if (!in.id())
        co_return RespondNotFound(request);
    Optional<ImageUpload> upload = ReadImageUpload(request, in);
    if (!upload)
        co_return;

    enum class Outcome { Stored, NotFound, NoDrive, Full, Failed };
    Outcome outcome = co_await session.OnWorker([upload = *upload, postId = *in.id(), user = request->GetAuth().User, settings = sWorld->GetSettings()]
    {
        Optional<PostAccess> access = WorkerPostAccess(postId, user);
        if (!access || !access->CanEdit())
            return Outcome::NotFound;
        Optional<UserRow> owner = FindUserById(access->OwnerId);
        if (!owner || owner->Deleted || !owner->IsGranted(GrantDrive))
            return Outcome::NoDrive;
        if (Drive::WorkerUsage(owner->Id).first + upload.Data.size() > Drive::QuotaOf(*owner, settings))
            return Outcome::Full;
        return WorkerStoreUpload(upload, settings.UploadDirectory, owner->Id, postId, user.Id) ? Outcome::Stored : Outcome::Failed;
    });
    switch (outcome)
    {
        case Outcome::NotFound:
            co_return RespondNotFound(request);
        case Outcome::NoDrive:
            co_return request->RespondError(http::status::forbidden, "owner_no_drive");
        case Outcome::Full:
            co_return request->RespondError(http::status::forbidden, "owner_drive_full");
        case Outcome::Failed:
            RT_LOG_ERROR("blog.admin", "Could not write the upload {} into Blog.Uploads.Directory", upload->Name);
            co_return RespondInternalError(request);
        case Outcome::Stored:
            break;
    }

    RT_LOG_INFO("blog.admin", "{} put {} ({}, {} bytes) in post {}", request->GetAuth().User.Username, upload->Name, upload->Mime,
        upload->Data.size(), *in.id());
    RespondUploaded(request, *upload);
}
}
