/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The API. Message types group routes for AntiDOS (Server.AntiDos.Limits in blog.yaml): auth.login
 * covers one route, posts.read every public read. Routes that carry passwords or session tokens are
 * never logged with their content. Most are made from the operations (ApiOperations.h); these here are
 * the routes that are HTTP's only: the login cookie, files, feeds.
 */

#include "BlogRoutes.h"
#include "ApiOperations.h"
#include "BlogSession.h"
#include "WorldHttpService.h"

namespace Blog
{
void RegisterRoutes(Rooftree::Server::WorldHttpService& service)
{
    using boost::beast::http::verb;
    using Rooftree::Net::Http::RequestHandlerFlag;
    constexpr RequestHandlerFlag secret = RequestHandlerFlag::DoNotLogRequestContent | RequestHandlerFlag::DoNotLogResponseContent;

    service.Route(verb::post, "/api/auth/oauth/{provider}", "auth.oauth", &BlogSession::HandleOAuthStart, RequestHandlerFlag::DoNotLogResponseContent);
    service.Route(verb::get, "/api/auth/oauth/{provider}/callback", "auth.oauth", &BlogSession::HandleOAuthCallback, secret);
    service.Route(verb::post, "/api/auth/oauth-totp", "auth.login", &BlogSession::HandleOAuthTotp, secret);
    service.Route(verb::post, "/api/auth/register", "auth.register", &BlogSession::HandleRegister, secret);
    service.Route(verb::post, "/api/auth/login", "auth.login", &BlogSession::HandleLogin, secret);

    service.Route(verb::post, "/api/unsubscribe", "auth.token", &BlogSession::HandleUnsubscribe, secret);

    service.Route(verb::get, "/api/feed.xml", "posts.read", &BlogSession::HandleFeed);
    service.Route(verb::get, "/api/sitemap.xml", "posts.read", &BlogSession::HandleSitemap);
    // Where readers find them
    service.Route(verb::get, "/{lang}/feed.xml", "posts.read", &BlogSession::HandleFeed);
    service.Route(verb::get, "/sitemap.xml", "posts.read", &BlogSession::HandleSitemap);
    service.Route(verb::get, "/robots.txt", "posts.read", &BlogSession::HandleRobots);

    // Downloads and pieces of uploads are neither logged with their content
    constexpr RequestHandlerFlag binary = RequestHandlerFlag::DoNotLogRequestContent | RequestHandlerFlag::DoNotLogResponseContent;
    service.Route(verb::get, "/api/drive/files/{id}", "drive", &BlogSession::HandleDriveDownload, binary);
    service.Route(verb::get, "/api/drive/zip", "drive", &BlogSession::HandleDriveZip, binary);
    service.Route(verb::get, "/api/drive/files/{id}/thumb", "drive", &BlogSession::HandleDriveThumb, binary);
    service.Route(verb::put, "/api/drive/files/{id}/thumb", "drive", &BlogSession::HandleDriveSetThumb, binary);
    service.Route(verb::put, "/api/drive/uploads/{id}", "drive", &BlogSession::HandleDriveUploadChunk, binary);
    service.Route(verb::get, "/api/shares/{token}/files/{id}", "drive.share", &BlogSession::HandleShareDownload, binary);
    service.Route(verb::get, "/api/shares/{token}/zip", "drive.share", &BlogSession::HandleShareZip, binary);
    service.Route(verb::get, "/api/shares/{token}/files/{id}/thumb", "drive.share", &BlogSession::HandleShareThumb, binary);

    // The operations', the readers' and the admin's
    for (ApiOperation const& operation : GetApiOperations())
        service.Route(operation.Method, operation.Path, operation.MessageType, [&operation](Rooftree::Server::Session& session,
            Rooftree::Server::WorldHttpRequestPtr const& request)
        {
            static_cast<BlogSession&>(session).RunOperation(request, operation);
        }, operation.Flags);
}
}
