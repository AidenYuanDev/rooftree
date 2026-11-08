/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The admin managing accounts: making one, changing its name, address, role (user or moderator) and
 * what it may do, its password and logins, deleting it. Only the admin; moderators get the list and
 * the bans (AdminHandlers.cpp).
 *
 * Admin accounts are not managed here, not even the admin's own: that is the account page and the
 * console. Every change forgets the account's cached logins, so its next request reads the new role and
 * permissions.
 *
 * Deleting comes in two kinds:
 *  - anonymize (the default): the comments stay, under "deleted account"; the row keeps only its id,
 *    nothing of the person (name, address, password, picture, logins, sessions, subscription)
 *  - purge: the row goes, and with it (ON DELETE CASCADE) its comments, the replies under its top-level
 *    comments too, and its reactions
 */

#include "Accounts.h"
#include "AdminUsers.h"
#include "Avatar.h"
#include "BlogDatabase.h"
#include "BlogSession.h"
#include "BlogWorld.h"
#include "Drive.h"
#include "Field.h"
#include "HttpRequestUtils.h"
#include "Log.h"
#include "PreparedStatement.h"
#include "QueryResult.h"
#include "SecurityUtils.h"
#include "StringConvert.h"
#include "Validation.h"
#include <boost/json/array.hpp>

namespace http = boost::beast::http;

using Rooftree::Server::Session;

namespace Blog
{
namespace
{
// The id of an admin call's table (ApiCall.h)
template<typename Table>
Optional<uint32> GetUserId(ApiRequest const& request)
{
    flatbuffers::Optional<uint32> id = request->Get<Table>().id();
    return id ? Optional<uint32>(*id) : Optional<uint32>();
}

// What the admin may set on an account, all optional (a PATCH changes what it names)
struct AccountFields
{
    Optional<std::string> Username;
    Optional<std::string> DisplayName;
    Optional<std::string> Email;
    Optional<bool> EmailVerified;
    Optional<std::string> Locale;
    Optional<std::string> Role;
    Optional<std::vector<std::string>> Denied;
    Optional<std::string> Password;
};

AccountFields ReadAccountFields(TableFields& fields, Admin::UserInput const& in, bool create)
{
    AccountFields account;
    if (create || in.username())
        account.Username = fields.Username("username", in.username());
    if (create || in.displayName())
        account.DisplayName = fields.Text("displayName", in.displayName(), 1, 40);
    if (create || in.email())
        account.Email = fields.Email("email", in.email());
    if (in.emailVerified())
        account.EmailVerified = *in.emailVerified();
    if (in.locale())
        account.Locale = fields.Lang("locale", in.locale());
    // Admins aren't made here: there is one, made with --create-admin
    if (in.role())
        account.Role = fields.Choice("role", in.role(), { RoleUser, RoleModerator });
    if (in.denied())
    {
        std::vector<std::string> denied = fields.SlugList("denied", in.denied(), std::size(DeniableActions), 10);
        if (std::ranges::any_of(denied, [](std::string const& action) { return std::ranges::find(DeniableActions, action) == std::end(DeniableActions); }))
            fields.AddError("denied", "invalid");
        account.Denied = std::move(denied);
    }
    // Optional on create as well: without one the account gets a mail to choose it
    if (in.password())
        account.Password = fields.Password("password", in.password());
    return account;
}


// Worker: which of username and email belong to another account than exceptId (0: any)
boost::json::object WorkerFindConflicts(std::string const& username, std::string const& email, uint32 exceptId)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_CONFLICTS_EXCEPT);
    stmt->setString(0, username);
    stmt->setString(1, email);
    stmt->setString(2, username);
    stmt->setString(3, email);
    stmt->setUInt32(4, exceptId);
    boost::json::object conflicts;
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* row = rows->Fetch();
            if (row[0].GetInt64())
                conflicts["username"] = "taken";
            if (row[1].GetInt64())
                conflicts["email"] = "taken";
        } while (rows->NextRow());
    }
    return conflicts;
}

// One account with everything its page shows
struct UserDetails
{
    AdminUserRow Row;
    int64 ReactionCount = 0;
    std::string Locale;
    bool HasPassword = false;

    struct Identity
    {
        std::string Provider;
        std::string Login;
        SystemTimePoint CreatedAt;
    };
    std::vector<Identity> Identities;

    struct LoginSession
    {
        std::string Hash;
        std::string Ip;
        std::string UserAgent;
        SystemTimePoint CreatedAt;
        SystemTimePoint LastSeenAt;
        SystemTimePoint ExpiresAt;
    };
    std::vector<LoginSession> Sessions;
};

Optional<UserDetails> WorkerLoadUserDetails(uint32 id)
{
    BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_ADMIN_USER);
    stmt->setUInt32(0, id);
    PreparedQueryResult row = BlogDatabase.Query(stmt);
    if (!row)
        return {};

    UserDetails details;
    Field* fields = row->Fetch();
    details.Row = AdminUserRow::FromFields(fields);
    details.ReactionCount = fields[20].GetInt64();
    details.Locale = fields[21].GetString();
    details.HasPassword = fields[22].GetInt64() != 0;

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_IDENTITIES);
    stmt->setUInt32(0, id);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* identity = rows->Fetch();
            details.Identities.push_back({ .Provider = identity[0].GetString(), .Login = identity[1].GetString(), .CreatedAt = identity[2].GetDate() });
        } while (rows->NextRow());
    }

    stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_SESSIONS);
    stmt->setUInt32(0, id);
    if (PreparedQueryResult rows = BlogDatabase.Query(stmt))
    {
        do
        {
            Field* session = rows->Fetch();
            details.Sessions.push_back({ .Hash = session[0].GetString(), .Ip = session[1].GetString(), .UserAgent = session[2].GetString(),
                .CreatedAt = session[3].GetDate(), .LastSeenAt = session[4].GetDate(), .ExpiresAt = session[5].GetDate() });
        } while (rows->NextRow());
    }

    return details;
}

// Main thread: with who is online now. currentSession: the admin's own, marked
void RespondUserDetails(ApiRequest const& request, UserDetails const& details, std::string_view currentSession)
{
    Optional<Presence::OnlineUser> online = sWorld->GetPresence().FindOnlineUser(details.Row.Id);
    flatbuffers::FlatBufferBuilder builder;
    std::vector<flatbuffers::Offset<Admin::UserIdentity>> identities;
    for (UserDetails::Identity const& identity : details.Identities)
        identities.push_back(Admin::CreateUserIdentity(builder, builder.CreateString(identity.Provider), builder.CreateString(identity.Login),
            TimeString(builder, identity.CreatedAt)));

    // A session's id is the hash of its cookie: it names the session, it can't log anyone in
    std::vector<flatbuffers::Offset<Admin::UserSession>> sessions;
    for (UserDetails::LoginSession const& session : details.Sessions)
        sessions.push_back(Admin::CreateUserSession(builder, builder.CreateString(session.Hash), builder.CreateString(session.Ip),
            builder.CreateString(session.UserAgent), TimeString(builder, session.CreatedAt), TimeString(builder, session.LastSeenAt),
            TimeString(builder, session.ExpiresAt), session.Hash == currentSession));

    AdminUserRow::Details extra{ .ReactionCount = uint32(details.ReactionCount), .Locale = builder.CreateString(details.Locale),
        .HasPassword = details.HasPassword, .Identities = builder.CreateVector(identities), .Sessions = builder.CreateVector(sessions) };
    builder.Finish(details.Row.BuildTable(builder, true, online ? &*online : nullptr, &extra));
    request->Respond(builder);
}

// Worker: the account of id when the admin may change it; error says why not
Optional<UserRow> WorkerFindManagedUser(uint32 id, std::string_view& error, bool allowDeleted = false)
{
    Optional<UserRow> user = FindUserById(id);
    if (!user || (user->Deleted && !allowDeleted))
        error = "not_found";
    else if (user->IsAdmin())
        error = "admin_account";
    return error.empty() ? user : Optional<UserRow>();
}

// A request or an admin call (ApiCall.h)
template<typename RequestPtr>
void RespondManagedError(RequestPtr const& request, std::string_view error)
{
    if (error == "not_found")
        return RespondNotFound(request);
    request->RespondError(http::status::forbidden, error);
}
}

Rooftree::Task<void> AdminCreateUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    TableFields fields(request->GetErrors());
    AccountFields account = ReadAccountFields(fields, request->Get<Admin::UserInput>(), true);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        boost::json::object Conflicts;
        bool Failed = false;
        Optional<UserRow> User;
        std::string Token;      // a link to choose a password, or to confirm the address
    };

    HashSettings hashSettings = GetHashSettings();
    Seconds tokenLifetime = sWorld->GetSettings().EmailVerificationLifetime;
    auto by = auth->User.Username;
    auto invite = !account.Password;
    Result result = co_await session.OnWorker([=]
    {
        Result result;
        result.Conflicts = WorkerFindConflicts(*account.Username, *account.Email, 0);
        if (!result.Conflicts.empty())
            return result;

        Optional<std::string> hash;
        if (account.Password)
        {
            hash = HashPassword(*account.Password, hashSettings);
            if (!hash)
            {
                result.Failed = true;
                return result;
            }
        }

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_INS_USER);
        stmt->setString(0, *account.Username);
        stmt->setString(1, *account.DisplayName);
        stmt->setString(2, *account.Email);
        if (hash)
            stmt->setString(3, *hash);
        else
            stmt->setNull(3);
        stmt->setString(4, account.Role.value_or(std::string(RoleUser)));
        stmt->setString(5, account.Locale.value_or("zh"));
        if (account.EmailVerified.value_or(false))
            stmt->setDate(6, SystemTimePoint::clock::now());
        else
            stmt->setNull(6);
        stmt->setNull(7);
        BlogDatabase.DirectExecute(stmt);

        stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_FOR_LOGIN);
        stmt->setString(0, *account.Username);
        stmt->setString(1, *account.Username);
        PreparedQueryResult row = BlogDatabase.Query(stmt);
        // Someone registered the name or address in between
        if (!row)
        {
            result.Conflicts = WorkerFindConflicts(*account.Username, *account.Email, 0);
            result.Failed = result.Conflicts.empty();
            return result;
        }
        result.User = UserRow::FromFields(row->Fetch());
        if (result.User->Email != *account.Email)
        {
            result.User.reset();
            result.Conflicts["username"] = "taken";
            return result;
        }

        if (account.Denied && !account.Denied->empty())
        {
            stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_BY_ADMIN);
            stmt->setString(0, result.User->Username);
            stmt->setString(1, result.User->DisplayName);
            stmt->setString(2, result.User->Email);
            stmt->setBool(3, result.User->EmailVerified);
            stmt->setString(4, result.User->Locale);
            stmt->setString(5, result.User->Role);
            stmt->setString(6, JoinList(*account.Denied));
            stmt->setUInt32(7, result.User->Id);
            BlogDatabase.DirectExecute(stmt);
        }

        if (!account.Password)
            result.Token = WorkerCreateEmailToken(result.User->Id, "reset", tokenLifetime);
        else if (!result.User->EmailVerified)
            result.Token = WorkerCreateEmailToken(result.User->Id, "verify", tokenLifetime);
        return result;
    });
    if (!result.Conflicts.empty())
        co_return request->RespondError(http::status::conflict, "conflict", {}, std::move(result.Conflicts));
    if (result.Failed || !result.User)
        co_return RespondInternalError(request);

    UserRow const& user = *result.User;
    RT_LOG_INFO("blog.admin", "{} made the account {} ({}), {}", by, user.Username, user.Id, user.Role);
    if (invite)
        sWorld->SendMail(session, InviteMail(user, result.Token, tokenLifetime));
    else if (!result.Token.empty())
        sWorld->SendMail(session, VerificationMail(user, result.Token));
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateUserCreated(builder, user.Id));
    request->Respond(builder, http::status::created);
}

Rooftree::Task<void> AdminGetUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::IdRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    auto sessionHash = auth->SessionHash;
    Optional<UserDetails> details = co_await session.OnWorker([id]
    {
        return WorkerLoadUserDetails(id);
    });
    if (!details)
        co_return RespondNotFound(request);
    RespondUserDetails(request, *details, sessionHash);
}

Rooftree::Task<void> AdminUpdateUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::UserInput>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    TableFields fields(request->GetErrors());
    AccountFields account = ReadAccountFields(fields, request->Get<Admin::UserInput>(), false);
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    struct Result
    {
        std::string_view Error;
        boost::json::object Conflicts;
        Optional<UserDetails> Details;
        std::vector<std::string> Changes;   // for the log
    };

    HashSettings hashSettings = GetHashSettings();
    auto by = auth->User.Username;
    auto sessionHash = auth->SessionHash;
    Result result = co_await session.OnWorker([=]
    {
        Result result;
        Optional<UserRow> user = WorkerFindManagedUser(id, result.Error);
        if (!user)
            return result;

        std::string username = account.Username.value_or(user->Username);
        std::string email = account.Email.value_or(user->Email);
        result.Conflicts = WorkerFindConflicts(username, email, id);
        if (!result.Conflicts.empty())
            return result;

        // A new address isn't verified, unless the admin says it is
        bool emailChanged = email != user->Email;
        bool verified = account.EmailVerified.value_or(emailChanged ? false : user->EmailVerified);
        std::vector<std::string> denied = account.Denied.value_or(user->Denied);
        std::string role = account.Role.value_or(user->Role);
        for (auto const& [name, changed] : std::initializer_list<std::pair<std::string_view, bool>>{
            { "username", username != user->Username },
            { "name", account.DisplayName && *account.DisplayName != user->DisplayName },
            { "email", emailChanged },
            { "verified", verified != user->EmailVerified },
            { "locale", account.Locale && *account.Locale != user->Locale },
            { "role", role != user->Role },
            { "denied", JoinList(denied) != JoinList(user->Denied) },
            { "password", account.Password.has_value() } })
            if (changed)
                result.Changes.emplace_back(name);

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_BY_ADMIN);
        stmt->setString(0, username);
        stmt->setString(1, account.DisplayName.value_or(user->DisplayName));
        stmt->setString(2, email);
        stmt->setBool(3, verified);
        stmt->setString(4, account.Locale.value_or(user->Locale));
        stmt->setString(5, role);
        stmt->setString(6, JoinList(denied));
        stmt->setUInt32(7, id);
        BlogDatabase.DirectExecute(stmt);

        // A new password logs it out everywhere, like a reset
        if (account.Password)
        {
            if (!WorkerSetPassword(id, *account.Password, hashSettings))
            {
                result.Error = "internal_error";
                return result;
            }
            WorkerDeleteUserSessions(id);
        }

        result.Details = WorkerLoadUserDetails(id);
        return result;
    });
    if (result.Error == "internal_error")
        co_return RespondInternalError(request);
    if (!result.Error.empty())
        co_return RespondManagedError(request, result.Error);
    if (!result.Conflicts.empty())
        co_return request->RespondError(http::status::conflict, "conflict", {}, std::move(result.Conflicts));
    if (!result.Details)
        co_return RespondNotFound(request);

    sWorld->ForgetUserAuth(id);
    if (!result.Changes.empty())
    {
        std::string changes;
        for (std::string const& change : result.Changes)
            changes += (changes.empty() ? "" : ", ") + change;
        RT_LOG_INFO("blog.admin", "{} changed {} of {} ({})", by, changes, result.Details->Row.Username, id);
    }
    RespondUserDetails(request, *result.Details, sessionHash);
}

Rooftree::Task<void> AdminDeleteUser(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::UsersDeleteRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    flatbuffers::String const* modeValue = request->Get<Admin::UsersDeleteRequest>().mode();
    std::string mode = modeValue ? modeValue->str() : std::string("anonymize");
    if (mode != "anonymize" && mode != "purge")
        co_return RespondValidation(request, { { "mode", "invalid" } });

    uint32 id = *idParam;
    bool purge = mode == "purge";
    struct Result
    {
        std::string_view Error;
        UserRow User;
    };

    auto by = auth->User.Username;
    Result result = co_await session.OnWorker([id, purge, directory = sWorld->GetSettings().UploadDirectory, driveDirectory = sWorld->GetSettings().DriveDirectory]
    {
        Result result;
        // An account deleted before, its comments kept, may still be purged
        Optional<UserRow> user = WorkerFindManagedUser(id, result.Error, purge);
        if (!user)
            return result;
        result.User = std::move(*user);

        // Its address isn't a subscriber's any more either
        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_SUBSCRIBER_BY_EMAIL);
        stmt->setString(0, result.User.Email);
        BlogDatabase.DirectExecute(stmt);

        if (purge)
        {
            // Posts and uploads keep their user (no ON DELETE): the admin's, moved elsewhere first
            stmt = BlogDatabase.GetPreparedStatement(BLOG_SEL_USER_OWNED_CONTENT);
            stmt->setUInt32(0, id);
            stmt->setUInt32(1, id);
            if (PreparedQueryResult owned = BlogDatabase.Query(stmt); owned && (owned->Fetch()[0].GetInt64() || owned->Fetch()[1].GetInt64()))
            {
                result.Error = "has_content";
                return result;
            }

            // Its drive and links first, while its rows say which files are its
            Drive::WorkerDeleteAccountDrive(id, driveDirectory);
            stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        else
        {
            Drive::WorkerDeleteAccountDrive(id, driveDirectory);
            for (BlogDatabaseStatements statement : { BLOG_DEL_USER_IDENTITIES, BLOG_DEL_USER_ALL_EMAIL_TOKENS, BLOG_DEL_USER_SESSIONS })
            {
                stmt = BlogDatabase.GetPreparedStatement(statement);
                stmt->setUInt32(0, id);
                BlogDatabase.DirectExecute(stmt);
            }

            stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_ANONYMIZE);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }

        RemoveAvatarFile(directory, result.User.Avatar);
        return result;
    });
    if (result.Error == "has_content")
        co_return request->RespondError(http::status::conflict, "has_content");
    if (!result.Error.empty())
        co_return RespondManagedError(request, result.Error);

    sWorld->ForgetUserAuth(result.User.Id);
    sWorld->RequestCommentCounts();
    // Its comments (and if purged, reactions) went from wherever they were
    sWorld->AnnounceChanged("*", "comments");
    if (purge)
        sWorld->AnnounceChanged("*", "reactions");
    RT_LOG_INFO("blog.admin", "{} {} the account {} ({})", by, purge ? "purged" : "deleted", result.User.Username, result.User.Id);
    RespondOk(request);
}

Rooftree::Task<void> AdminMailPasswordReset(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::IdRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    struct Result
    {
        std::string_view Error;
        Optional<UserRow> User;
        std::string Token;
    };

    auto by = auth->User.Username;
    Result result = co_await session.OnWorker([id, lifetime = sWorld->GetSettings().PasswordResetLifetime]
    {
        Result result;
        result.User = WorkerFindManagedUser(id, result.Error);
        if (result.User)
            result.Token = WorkerCreateEmailToken(id, "reset", lifetime);
        return result;
    });
    if (!result.Error.empty())
        co_return RespondManagedError(request, result.Error);

    RT_LOG_INFO("blog.admin", "{} mailed a password reset link to {} ({})", by, result.User->Username, result.User->Id);
    sWorld->SendMail(session, ResetMail(*result.User, result.Token));
    RespondOk(request);
}

Rooftree::Task<void> AdminDisableTotp(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::IdRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    auto by = auth->User.Username;
    std::string_view error = co_await session.OnWorker([id]
    {
        std::string_view error;
        if (WorkerFindManagedUser(id, error))
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_TOTP_DISABLE);
            stmt->setUInt32(0, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return error;
    });
    if (!error.empty())
        co_return RespondManagedError(request, error);

    RT_LOG_INFO("blog.admin", "{} turned off two-step login of user {}", by, id);
    sWorld->UpdateCachedUser(id, [](UserRow& user) { user.TotpSecret.reset(); });
    RespondOk(request);
}

Rooftree::Task<void> AdminDeleteSessions(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::IdRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    auto by = auth->User.Username;
    std::string_view error = co_await session.OnWorker([id]
    {
        std::string_view error;
        if (WorkerFindManagedUser(id, error))
            WorkerDeleteUserSessions(id);
        return error;
    });
    if (!error.empty())
        co_return RespondManagedError(request, error);

    RT_LOG_INFO("blog.admin", "{} logged user {} out everywhere", by, id);
    sWorld->ForgetUserAuth(id);
    RespondOk(request);
}

Rooftree::Task<void> AdminDeleteSession(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::UsersDeleteSessionRequest>(request);
    flatbuffers::String const* hashValue = request->Get<Admin::UsersDeleteSessionRequest>().session();
    std::string hash = hashValue ? hashValue->str() : std::string();
    if (!idParam || hash.size() != 64)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    auto by = auth->User.Username;
    std::string_view error = co_await session.OnWorker([id, hash]
    {
        std::string_view error;
        if (WorkerFindManagedUser(id, error))
        {
            BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_DEL_USER_SESSION);
            stmt->setString(0, hash);
            stmt->setUInt32(1, id);
            BlogDatabase.DirectExecute(stmt);
        }
        return error;
    });
    if (!error.empty())
        co_return RespondManagedError(request, error);

    RT_LOG_INFO("blog.admin", "{} ended a login session of user {}", by, id);
    // Only when it is this user's: another user's session would be found again in the database
    if (AuthInfo const* cached = sWorld->FindAuth(hash); cached && cached->User.Id == id)
        sWorld->ForgetAuth(hash);
    RespondOk(request);
}

Rooftree::Task<void> AdminOnline(Session& session, ApiRequest request)
{
    Presence& presence = sWorld->GetPresence();
    flatbuffers::FlatBufferBuilder builder;
    builder.Finish(Admin::CreateOnlineCounts(builder, uint32(presence.GetViewerCount()), uint32(presence.GetOnlineUsers().size())));
    request->Respond(builder);
    co_return;
}

// {enabled, quota, trashDays}: the account's drive on or off, its own size in bytes and days of trash
// (null for the defaults). Turned off, its files stay but nobody reaches them, its links neither, until
// it is on again
Rooftree::Task<void> AdminSetUserDrive(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> idParam = GetUserId<Admin::UsersSetDriveRequest>(request);
    if (!idParam)
        co_return RespondNotFound(request);

    uint32 id = *idParam;
    Admin::UsersSetDriveRequest const& in = request->Get<Admin::UsersSetDriveRequest>();
    TableFields fields(request->GetErrors());
    Optional<bool> enabled = in.enabled() ? Optional<bool>(*in.enabled()) : Optional<bool>();
    if (!enabled)
        fields.AddError("enabled", "required");
    Optional<uint64> quota;
    if (in.quota())
    {
        // At most a TiB: more is a typo
        if (*in.quota() <= (uint64(1) << 40))
            quota = *in.quota();
        else
            fields.AddError("quota", "invalid");
    }
    // Days its trash keeps what it deleted; none: the default
    Optional<uint32> trashDays;
    if (in.trashDays())
    {
        if (*in.trashDays() <= Drive::MaxTrashDays)
            trashDays = *in.trashDays();
        else
            fields.AddError("trashDays", "invalid");
    }
    if (!fields.IsValid())
        co_return RespondValidation(request, fields.TakeErrors());

    auto by = auth->User.Username;
    auto sessionHash = auth->SessionHash;
    Optional<UserDetails> details = co_await session.OnWorker([id, enabled = *enabled, quota, trashDays]() -> Optional<UserDetails>
    {
        Optional<UserRow> user = FindUserById(id);
        if (!user || user->Deleted)
            return {};

        std::vector<std::string> granted;
        for (std::string const& action : user->Granted)
            if (action != GrantDrive)
                granted.push_back(action);
        if (enabled)
            granted.emplace_back(GrantDrive);

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_DRIVE);
        stmt->setString(0, JoinList(granted));
        if (quota)
            stmt->setUInt64(1, *quota);
        else
            stmt->setNull(1);
        if (trashDays)
            stmt->setUInt32(2, *trashDays);
        else
            stmt->setNull(2);
        stmt->setUInt32(3, id);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadUserDetails(id);
    });
    if (!details)
        co_return RespondNotFound(request);

    sWorld->ForgetUserAuth(id);
    RT_LOG_INFO("blog.admin", "{} set the drive of {} ({}): {}, {}, {}", by, details->Row.Username, id, *enabled ? "on" : "off",
        quota ? std::to_string(*quota) + " bytes" : std::string("default size"),
        trashDays ? std::to_string(*trashDays) + " days of trash" : std::string("default trash"));
    RespondUserDetails(request, *details, sessionHash);
}

Rooftree::Task<void> AdminSetUserWrite(Session& session, ApiRequest request)
{
    AuthInfo const* auth = &request->GetAuth();
    Optional<uint32> id = GetUserId<Admin::UsersSetWriteRequest>(request);
    if (!id)
        co_return RespondNotFound(request);

    flatbuffers::Optional<bool> enabled = request->Get<Admin::UsersSetWriteRequest>().enabled();
    if (!enabled)
        co_return RespondValidation(request, { { "enabled", "required" } });

    auto by = auth->User.Username;
    auto sessionHash = auth->SessionHash;
    Optional<UserDetails> details = co_await session.OnWorker([id = *id, enabled = *enabled]() -> Optional<UserDetails>
    {
        Optional<UserRow> user = FindUserById(id);
        if (!user || user->Deleted)
            return {};

        std::vector<std::string> granted;
        for (std::string const& action : user->Granted)
            if (action != GrantWrite)
                granted.push_back(action);
        if (enabled)
            granted.emplace_back(GrantWrite);

        BlogDatabasePreparedStatement* stmt = BlogDatabase.GetPreparedStatement(BLOG_UPD_USER_GRANTED);
        stmt->setString(0, JoinList(granted));
        stmt->setUInt32(1, id);
        BlogDatabase.DirectExecute(stmt);
        return WorkerLoadUserDetails(id);
    });
    if (!details)
        co_return RespondNotFound(request);

    sWorld->ForgetUserAuth(*id);
    RT_LOG_INFO("blog.admin", "{} {} writing to {} ({})", by, *enabled ? "gave" : "took", details->Row.Username, *id);
    RespondUserDetails(request, *details, sessionHash);
}
}
