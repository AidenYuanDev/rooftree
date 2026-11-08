/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * What an operation of the API does, in FlatBuffers: its route, its packet (ApiPackets.h) and, for the
 * admin's, the console's commands (ConsoleAdmin.cpp) run the same function, an ApiOp, found by its row
 * in ApiOperations.h. It gets an ApiCall holding its request table (schema/Admin.fbs, Reader.fbs) and
 * who is calling, and answers with its response table or an error:
 *
 *   Admin::TagInput const& input = request->Get<Admin::TagInput>();
 *   TableFields fields(request->GetErrors());
 *   std::string slug = fields.Slug("slug", input.slug(), MaxTagSlugLength);
 *   ...
 *   flatbuffers::FlatBufferBuilder builder;
 *   builder.Finish(TagTable(builder, *tag));
 *   request->Respond(builder, http::status::created);
 *
 * A packet's table is its payload. A route's and the console's are made from their JSON at the edge
 * (ApiJson.h), which also turns the answer back into JSON; what of that JSON didn't fit the table
 * (a number for a string) is in GetErrors(), for the handler's 400 to name with the rest.
 *
 * The caller has checked who may call it, as its row says (ApiAccess): RequireUser and the like for a
 * request, the login of the connection for a packet, nothing for the console, which calls as the
 * admin. An operation anyone may call has a login or not (GetLogin).
 */

#ifndef BLOG_API_CALL_H
#define BLOG_API_CALL_H

#include "Admin_generated.h"
#include "BlogTypes.h"
#include "Packet.h"
#include "Reader_generated.h"
#include "Task.h"
#include "Validation.h"
#include <boost/asio/ip/address.hpp>
#include <boost/beast/http/status.hpp>
#include <boost/json/object.hpp>
#include <atomic>
#include <functional>
#include <memory>

namespace Rooftree::Server
{
class Session;
}

namespace Blog
{
// What an operation did to the login it was called with, for its edge to carry out with the answer: a
// route clears or sets the cookie, a packet's connection calls as the new login from then on
struct LoginChange
{
    bool Ended = false;             // logged out
    std::string NewToken;           // a new session of the same account (the cookie's value), its hash
    std::string NewHash;
    Optional<SystemTimePoint> NewExpiresAt;
};

class ApiCall
{
public:
    // Main thread, once: the response table (a status below 300) or a Rooftree.Packets.Error, and what
    // the operation did to the caller's login
    using Done = std::function<void(boost::beast::http::status status, std::string payload, LoginChange const& login)>;

    struct Params
    {
        Optional<AuthInfo> Auth;        // none: anonymous
        uint16 Opcode = 0;              // the operation's, for its errors
        std::string Request;            // a verified buffer of the operation's request table
        FieldErrors Errors;             // of the JSON it was made from
        boost::asio::ip::address ClientAddress;
        std::string UserAgent;          // the request's, or the handshake's of a packet's connection
    };

    ApiCall(Params params, Done done) : _params(std::move(params)), _done(std::move(done)) { }
    ApiCall(ApiCall const&) = delete;
    ApiCall& operator=(ApiCall const&) = delete;
    // Never answered: 500, like a request
    ~ApiCall();

    // An operation that needs a login has one (asserted)
    AuthInfo const& GetAuth() const;
    // The login, null without one
    AuthInfo const* GetLogin() const { return _params.Auth ? &*_params.Auth : nullptr; }
    boost::asio::ip::address const& GetClientAddress() const { return _params.ClientAddress; }
    std::string const& GetUserAgent() const { return _params.UserAgent; }
    uint16 GetOpcode() const { return _params.Opcode; }

    // The request table, of the operation's type
    template<typename Table>
    Table const& Get() const { return *flatbuffers::GetRoot<Table>(_params.Request.data()); }
    FieldErrors const& GetErrors() const { return _params.Errors; }

    // A finished buffer of the operation's response table, or its bytes (a worker's, Bytes())
    void Respond(flatbuffers::FlatBufferBuilder const& builder, boost::beast::http::status status = boost::beast::http::status::ok);
    void Respond(std::string payload, boost::beast::http::status status = boost::beast::http::status::ok);
    // fields: a validation_failed's or conflict's {"slug": "taken"}
    void RespondError(boost::beast::http::status status, std::string_view code, std::string_view message = {}, boost::json::object const& fields = {});
    // With what more it says (Packet.h): its seconds to wait, its details
    void RespondError(Rooftree::Server::PacketError const& error);
    bool HasResponded() const { return _responded; }

    // Before answering: the caller's login ended, or goes on as a new session (a password change ends
    // every session of the account)
    void EndLogin() { _login.Ended = true; }
    void StartLogin(std::string token, std::string hash, SystemTimePoint expiresAt)
    {
        _login.NewToken = std::move(token);
        _login.NewHash = std::move(hash);
        _login.NewExpiresAt = expiresAt;
    }

private:
    void Answer(boost::beast::http::status status, std::string payload);

    Params _params;
    Done _done;
    LoginChange _login;
    std::atomic<bool> _responded = false;
};

using ApiRequest = std::shared_ptr<ApiCall>;
// A coroutine: Session::Spawn runs it
using ApiOp = Rooftree::Task<void> (*)(Rooftree::Server::Session& session, ApiRequest request);

// Like the request's ones (BlogSession.h)
void RespondValidation(ApiRequest const& request, boost::json::object fields);
void RespondNotFound(ApiRequest const& request);
void RespondInternalError(ApiRequest const& request);
// {ok: true}, for what has nothing more to say: the Ok of the operation's schema (Admin.fbs, Reader.fbs)
void RespondOk(ApiRequest const& request);
// 403 banned, until when and why
void RespondBanned(ApiRequest const& request, UserRow const& user);
// A post or note there, not in the language asked for: 404 not_in_lang naming the languages it has
void RespondNotInLang(ApiRequest const& request, std::vector<std::string> const& langs);

// What RespondBanned says, for the edges that check a login before an operation runs
Rooftree::Server::PacketError BannedError(UserRow const& user);

// The picture captcha of a request that mails someone, once its other fields are valid: adds the field
// error captcha "required" or "wrong" unless it is right. Always right with Blog.Captcha.Enable off
bool CheckCaptcha(TableFields& fields, flatbuffers::String const* id, flatbuffers::String const* answer);
// One mail to an address per Blog.Auth.MailInterval; answers 429 too_many_mails, when to retry, when
// it is too soon
bool CheckMailInterval(ApiRequest const& request, std::string const& email);

// A request's lang: zh unless en
std::string ReadLang(flatbuffers::String const* lang);
// Its text, "" when it wasn't sent
std::string ReadText(flatbuffers::String const* text);

// A finished buffer's bytes: what a worker that built an answer hands back (builders don't copy)
std::string Bytes(flatbuffers::FlatBufferBuilder const& builder);

// For answers: an ISO 8601 time, absent (null) for none
flatbuffers::Offset<flatbuffers::String> TimeString(flatbuffers::FlatBufferBuilder& builder, Optional<SystemTimePoint> time);
flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> StringList(flatbuffers::FlatBufferBuilder& builder,
    std::vector<std::string> const& values);
// Whether a table of a request has nothing set: an empty language is one to delete (json_null)
bool IsEmptyTable(flatbuffers::Table const* table);
// A table of generated code, which is a flatbuffers::Table (privately)
template<typename Table>
bool IsEmptyTable(Table const* table) { return IsEmptyTable(reinterpret_cast<flatbuffers::Table const*>(table)); }

// The readers' operations, by file
// UserFriendHandlers.cpp
Rooftree::Task<void> ListUserFriends(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListUserFriendRequests(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> SendUserFriendRequest(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AcceptUserFriendRequest(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> RejectUserFriendRequest(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> RemoveUserFriend(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetUserFriendshipStatus(Rooftree::Server::Session& session, ApiRequest request);

// ChatHandlers.cpp
Rooftree::Task<void> ListChatChannels(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListChatMessages(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> SendChatMessage(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> MarkChatRead(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetChatUnreadCount(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CreateChatGroup(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListChatGroupMembers(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> LeaveChatGroup(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminListChatGroups(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminListChatMessages(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteChatMessage(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminClearChatChannel(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteChatGroup(Rooftree::Server::Session& session, ApiRequest request);

// PostHandlers.cpp
Rooftree::Task<void> ListPosts(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetPost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListSlugs(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> SearchPosts(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListTags(Rooftree::Server::Session& session, ApiRequest request);
// ContentHandlers.cpp
Rooftree::Task<void> ListNotes(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetNote(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListThinking(Rooftree::Server::Session& session, ApiRequest request);
// SiteHandlers.cpp
Rooftree::Task<void> GetSite(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetAbout(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListTimeline(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListFriends(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ApplyFriend(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListProjects(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListUploadsMeta(Rooftree::Server::Session& session, ApiRequest request);
// CommentHandlers.cpp
Rooftree::Task<void> ListComments(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListAnchoredComments(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CreateComment(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UpdateComment(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteComment(Rooftree::Server::Session& session, ApiRequest request);
// ReactionHandlers.cpp
Rooftree::Task<void> ListReactions(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> React(Rooftree::Server::Session& session, ApiRequest request);
// StatsHandlers.cpp
Rooftree::Task<void> CountView(Rooftree::Server::Session& session, ApiRequest request);
// NewsletterHandlers.cpp
Rooftree::Task<void> Subscribe(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ConfirmSubscription(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetSubscription(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> SetSubscription(Rooftree::Server::Session& session, ApiRequest request);
// AuthHandlers.cpp
Rooftree::Task<void> GetCaptcha(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListOAuthProviders(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetMe(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UpdateMe(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> VerifyEmail(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ResendVerification(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ForgotPassword(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ResetPassword(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ChangePassword(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> Logout(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> SetupTotp(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> EnableTotp(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DisableTotp(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListIdentities(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteIdentity(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UploadAvatar(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UseProviderAvatar(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteAvatar(Rooftree::Server::Session& session, ApiRequest request);
// DriveHandlers.cpp
Rooftree::Task<void> ListDrive(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListDriveFolders(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CreateDriveFolder(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UpdateDriveFolder(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteDriveFolder(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UpdateDriveFile(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteDriveFile(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListDriveTrash(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> RestoreFromTrash(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> PurgeFromTrash(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> EmptyDriveTrash(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CreateDriveUpload(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CompleteDriveUpload(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CancelDriveUpload(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListDriveShares(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> CreateDriveShare(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> ListDriveShareAccess(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> DeleteDriveShare(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetShare(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> UnlockShare(Rooftree::Server::Session& session, ApiRequest request);

// The admin routes' operations, by file
// PostHandlers.cpp
Rooftree::Task<void> AdminListPosts(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminGetPost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminPostRevisions(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminPostRevision(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminSavePostVersion(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminRestorePostRevision(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreatePost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdatePost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeletePost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminPostEditors(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminPublishPost(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminPostUpload(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> GetPerson(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminSetPostEditor(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminRemovePostEditor(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreateTag(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateTag(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteTag(Rooftree::Server::Session& session, ApiRequest request);
// ContentHandlers.cpp
Rooftree::Task<void> AdminListNotes(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminGetNote(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreateNote(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateNote(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteNote(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminListThinking(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminGetThinking(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreateThinking(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateThinking(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteThinking(Rooftree::Server::Session& session, ApiRequest request);
// SiteHandlers.cpp
Rooftree::Task<void> AdminStatus(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminListFriends(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreateFriend(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateFriend(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteFriend(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminCreateProject(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateProject(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteProject(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminGetSite(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateSite(Rooftree::Server::Session& session, ApiRequest request);
// AdminHandlers.cpp
Rooftree::Task<void> AdminListUsers(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminBanUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUnbanUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminListIpBans(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminAddIpBan(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteIpBan(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpload(Rooftree::Server::Session& session, ApiRequest request);
// UserAdminHandlers.cpp
Rooftree::Task<void> AdminCreateUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminGetUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminUpdateUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteUser(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminMailPasswordReset(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDisableTotp(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteSessions(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminDeleteSession(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminOnline(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminSetUserDrive(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminSetUserWrite(Rooftree::Server::Session& session, ApiRequest request);
// StatsHandlers.cpp
Rooftree::Task<void> AdminStats(Rooftree::Server::Session& session, ApiRequest request);
// CommentHandlers.cpp
Rooftree::Task<void> AdminListComments(Rooftree::Server::Session& session, ApiRequest request);
Rooftree::Task<void> AdminModerateComment(Rooftree::Server::Session& session, ApiRequest request);
// DriveHandlers.cpp
Rooftree::Task<void> AdminDrives(Rooftree::Server::Session& session, ApiRequest request);
}

#endif // BLOG_API_CALL_H
