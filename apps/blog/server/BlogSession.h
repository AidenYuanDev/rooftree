/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The session of one client address. Handlers run on the main thread; the
 * ones that need several dependent queries or Argon2 hand that to a worker with co_await OnWorker and
 * respond when it is back.
 *
 * Most of the API are operations (ApiOperations.h): one function per route and packet, which reads
 * and answers FlatBuffers (ApiCall.h). The routes here are HTTP's only: what sets the login cookie,
 * files, feeds, the one-click unsubscribe.
 *
 * Who is logged in is found per request from the session cookie (Authenticate). Handlers that change
 * something take RequireUser/RequireAdmin, which also check the request's Origin and CSRF token;
 * anonymous ones that change something (register, log in, reset) check the Origin only.
 *
 * Both are split by area: AuthHandlers.cpp, PostHandlers.cpp (also search and feeds),
 * ContentHandlers.cpp (notes, thinking), CommentHandlers.cpp, ReactionHandlers.cpp,
 * NewsletterHandlers.cpp (subscriptions), SiteHandlers.cpp (profile, timeline, friends, projects),
 * AdminHandlers.cpp (also uploads), DriveHandlers.cpp (accounts' drives and their links).
 */

#ifndef BLOG_SESSION_H
#define BLOG_SESSION_H

#include "ApiCall.h"
#include "BlogTypes.h"
#include "ServerSession.h"
#include "WorldHttpService.h"
#include "WorldWebSocket.h"
#include <functional>

namespace Blog
{
struct ApiOperation;
struct IpBan;
class RequestFields;
struct TagInfo;

class BlogSession final : public Rooftree::Server::Session
{
public:
    using Session::Session;

    using Request = Rooftree::Server::WorldHttpRequestPtr;
    // auth is only valid during the call
    using AuthHandler = std::function<void(AuthInfo const* auth)>;

    // auth or null, never fails
    void Authenticate(Request const& request, AuthHandler next);
    // The login of a session cookie's hash, from the cache or the database (once); null when there is
    // none. For the WebSocket, which has the cookie of its handshake and no request
    void FindLogin(std::string const& sessionHash, AuthHandler next);

    // 401 without a login, 403 when banned (unless allowBanned) or when the request changes something
    // without our Origin and the session's CSRF token
    void RequireUser(Request const& request, AuthHandler next, bool allowBanned = false);
    void RequireAdmin(Request const& request, AuthHandler next);
    // RequireUser as a coroutine: the login, nothing when it answered the request
    Rooftree::Task<Optional<AuthInfo>> RequireUserAsync(Request request, bool allowBanned = false);
    // The drive a request is for (DriveHandlers.cpp): the login's own, 403 no_drive without one; with
    // ?user=<id> that account's, for the admin only. Nothing when it answered the request
    Rooftree::Task<Optional<UserRow>> RequireDrive(Request request);
    // The admin or a moderator
    void RequireModerator(Request const& request, AuthHandler next);

    // A route's operation (ApiOperations.h) for the request, after Authenticate, RequireUser and the
    // like as the operation says
    void RunOperation(Request const& request, ApiOperation const& operation);

    // A state-changing anonymous request comes from one of our origins; responds 403 if not
    bool CheckOrigin(Request const& request);

    // The picture captcha of a request that mails someone (captchaId and captcha in its body), once
    // its other fields are valid: adds the field error captcha "required" or "wrong" unless it is
    // right. Always right with Blog.Captcha.Enable off
    bool CheckCaptcha(RequestFields& fields);

    // what sets the login cookie, GitHub and Google logins (AuthHandlers.cpp)
    Rooftree::Task<void> HandleRegister(Request request);
    Rooftree::Task<void> HandleLogin(Request request);
    void HandleOAuthStart(Request const& request);
    Rooftree::Task<void> HandleOAuthCallback(Request request);
    Rooftree::Task<void> HandleOAuthTotp(Request request);

    // WebSocket (BlogWebSocket.cpp)
    void HandleWsPage(Rooftree::Server::WebSocketJsonMessage& message);
    // A post's editors writing together (Collab.h)
    void HandleWsDoc(Rooftree::Server::WebSocketJsonMessage& message);

    // a mail's one-click unsubscribe, a form (RFC 8058)
    Rooftree::Task<void> HandleUnsubscribe(Request request);

    // feeds
    Rooftree::Task<void> HandleFeed(Request request);
    Rooftree::Task<void> HandleSitemap(Request request);
    void HandleRobots(Request const& request);

    // the drive's files and pieces of uploads (DriveHandlers.cpp)
    Rooftree::Task<void> HandleDriveDownload(Request request);
    Rooftree::Task<void> HandleDriveZip(Request request);
    Rooftree::Task<void> HandleDriveThumb(Request request);
    Rooftree::Task<void> HandleDriveSetThumb(Request request);
    Rooftree::Task<void> HandleDriveUploadChunk(Request request);
    // what a link shows, to any account
    Rooftree::Task<void> HandleShareDownload(Request request);
    Rooftree::Task<void> HandleShareZip(Request request);
    Rooftree::Task<void> HandleShareThumb(Request request);

    // Starts a new login session for user: the cookie on the response, the session in the cache.
    // Returns the CSRF token. Worker thread part (the INSERT) is done by the caller: see AuthHandlers.cpp
    std::string StartAuthSession(Request const& request, UserRow const& user, std::string const& token, std::string const& sessionHash,
        std::string const& csrfToken, SystemTimePoint expiresAt);
};

// Startup, before the network: the tags, the IP bans and the site's profile with blocking queries
std::vector<TagInfo> LoadTagsDirect();
std::vector<IpBan> LoadIpBansDirect();
boost::json::object LoadSiteProfileDirect();

// The site's profile, from the admin's page or the console: validated and normalized like the page
// sends it (nothing and errors when not), saved on a worker, then the cache and a rebuild
Optional<boost::json::object> ReadSiteProfile(boost::json::object const& body, boost::json::object& errors);
void WorkerSaveSiteProfile(std::string const& json);
void ApplySiteProfile(boost::json::object profile);

// Shared by the handler files
// ?lang=, zh unless en
std::string GetLang(BlogSession::Request const& request);
// Lists by (published_at, id), newest first: the cursor names the last row of a page
std::string EncodeCursor(SystemTimePoint publishedAt, uint32 id);
Optional<std::pair<SystemTimePoint, uint32>> DecodeCursor(std::string_view cursor);
// Startup: word counts of rows from before there were any
void CountMissingWordsDirect();
void RespondValidation(BlogSession::Request const& request, boost::json::object fields);
void RespondNotFound(BlogSession::Request const& request);
void RespondInternalError(BlogSession::Request const& request);
}

#endif // BLOG_SESSION_H
