/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * What nginx used to answer, now answered on the network threads (StaticSite.h): the prerendered site
 * from memory, the uploads from the disk, the redirects. None of it reaches the main thread, so a page
 * with its forty scripts and fonts costs the API nothing.
 *
 *   /                          302 to /zh or /en, by Accept-Language
 *   /feed.xml                  301 to /zh/feed.xml
 *   /.well-known/acme-challenge/<token>    certbot's answer (--webroot) when it renews the certificate
 *   /uploads/<name>            the image, cached for a year (names never change)
 *   /assets/...                built files, hashed names: cached for a year, 404 when missing
 *   /zh/posts/-, .../-.data    the placeholders that keep a build without posts valid: 404
 *   ....data                   data of client-side navigation: 404 when missing, never the HTML fallback
 *   anything else              the file, else <path>/index.html, else the SPA fallback (admin, search)
 *
 * Pages and their data are "no-cache": the browser asks every time, and gets a 304 while the build is
 * the same (ETag).
 */

#include "BlogRoutes.h"
#include "JsonHttp.h"
#include "StaticSite.h"
#include "WorldHttpService.h"
#include <algorithm>

namespace http = boost::beast::http;

namespace Blog
{
namespace
{
using Rooftree::Net::Http::RequestContext;
using Rooftree::Net::Http::RequestHandlerResult;

constexpr std::string_view NoCache = "no-cache";
constexpr std::string_view Immutable = "public, max-age=31536000, immutable";
constexpr std::string_view SpaFallback = "/__spa-fallback.html";

void Redirect(RequestContext& context, http::status status, std::string_view location)
{
    context.response.result(status);
    context.response.set(http::field::location, location);
}

// The SPA fallback renders the 404 page, with a 404
void NotFound(RequestContext& context, Rooftree::Server::StaticSite const* site)
{
    if (Rooftree::Server::StaticFile const* page = site ? site->Find(SpaFallback) : nullptr)
        Rooftree::Server::SetStaticResponse(context, *page, NoCache, http::status::not_found);
    else
        context.response.result(http::status::not_found);
}

bool IsPlaceholder(std::string_view path)
{
    // /(zh|en)/(posts|tags|notes)/-, with / or .data after it
    for (std::string_view lang : { "/zh/", "/en/" })
        for (std::string_view kind : { "posts/-", "tags/-", "notes/-" })
        {
            std::string prefix = std::string(lang) + std::string(kind);
            if (path == prefix || path == prefix + "/" || path == prefix + ".data")
                return true;
        }
    return false;
}

void ServeSite(RequestContext& context, std::string_view path, Rooftree::Server::StaticSite const* site)
{
    if (!site || IsPlaceholder(path))
        return NotFound(context, site);

    if (path.starts_with("/assets/"))
    {
        if (Rooftree::Server::StaticFile const* file = site->Find(path))
            return Rooftree::Server::SetStaticResponse(context, *file, Immutable);
        return NotFound(context, site);
    }

    if (path.ends_with(".data"))
    {
        if (Rooftree::Server::StaticFile const* file = site->Find(path))
            return Rooftree::Server::SetStaticResponse(context, *file, NoCache);
        return NotFound(context, site);
    }

    // try_files $uri $uri/index.html /__spa-fallback.html
    std::string_view directory = path.ends_with('/') ? path.substr(0, path.size() - 1) : path;
    Rooftree::Server::StaticFile const* file = path.ends_with('/') ? nullptr : site->Find(path);
    if (!file)
        file = site->Find(std::string(directory) + "/index.html");
    if (!file)
        file = site->Find(SpaFallback);
    if (!file)
        return NotFound(context, site);
    Rooftree::Server::SetStaticResponse(context, *file, NoCache);
}

bool IsUploadName(std::string_view name)
{
    std::size_t dot = name.find('.');
    if (dot == 0 || dot == std::string_view::npos)
        return false;

    std::string_view extension = name.substr(dot + 1);
    if (extension != "jpg" && extension != "png" && extension != "gif" && extension != "webp")
        return false;

    // Only our own names: base64url and one of our extensions, nothing that walks the file system
    return std::ranges::all_of(name.substr(0, dot), [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    });
}
}

void RegisterStaticRoutes(Rooftree::Server::WorldHttpService& service, Rooftree::Server::StaticSiteHolder const& site,
    std::string uploadDirectory, std::string acmeDirectory)
{
    using boost::beast::http::verb;
    using Socket = std::shared_ptr<Rooftree::Server::WorldHttpSocket>;

    // English when it comes first in Accept-Language, else Chinese
    service.RegisterHandler(verb::get, "/", [](Socket const&, RequestContext& context)
    {
        std::string_view language = Rooftree::Net::Http::ToStdStringView(context.request[http::field::accept_language]);
        bool english = language.size() >= 2 && (language[0] == 'e' || language[0] == 'E') && (language[1] == 'n' || language[1] == 'N');
        Redirect(context, http::status::found, english ? "/en" : "/zh");
        context.response.set(http::field::vary, "Accept-Language");
        return RequestHandlerResult::Handled;
    });

    service.RegisterHandler(verb::get, "/feed.xml", [](Socket const&, RequestContext& context)
    {
        Redirect(context, http::status::moved_permanently, "/zh/feed.xml");
        return RequestHandlerResult::Handled;
    });

    // certbot --webroot -w <acmeDirectory> writes the token's file under .well-known/acme-challenge
    if (!acmeDirectory.empty())
        service.RegisterHandler(verb::get, "/.well-known/acme-challenge/{token}", [acmeDirectory = std::move(acmeDirectory)](Socket const&, RequestContext& context)
        {
            std::string_view token = context.pathParameters.Get("token");
            bool valid = std::ranges::all_of(token, [](char c)
            {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            });
            Optional<std::string> data = valid ? Rooftree::Server::ReadWholeFile(acmeDirectory + "/.well-known/acme-challenge/" + std::string(token))
                : Optional<std::string>();
            if (!data)
            {
                context.response.result(http::status::not_found);
                return RequestHandlerResult::Handled;
            }

            context.response.result(http::status::ok);
            context.response.set(http::field::content_type, "text/plain");
            context.response.body() = std::move(*data);
            return RequestHandlerResult::Handled;
        });

    // The directory as it was at startup: a reload doesn't move the uploads
    service.RegisterHandler(verb::get, "/uploads/{name}", [uploadDirectory = std::move(uploadDirectory)](Socket const&, RequestContext& context)
    {
        std::string_view name = context.pathParameters.Get("name");
        Optional<std::string> data = IsUploadName(name) ? Rooftree::Server::ReadWholeFile(uploadDirectory + "/" + std::string(name)) : Optional<std::string>();
        if (!data)
        {
            context.response.result(http::status::not_found);
            return RequestHandlerResult::Handled;
        }

        context.response.result(http::status::ok);
        context.response.set(http::field::content_type, Rooftree::Server::GetContentType(name));
        context.response.set(http::field::cache_control, Immutable);
        context.response.body() = std::move(*data);
        return RequestHandlerResult::Handled;
    });

    service.SetFallbackHandler([&site](std::shared_ptr<Rooftree::Net::Http::AbstractSocket> const&, RequestContext& context)
    {
        Optional<std::string> path = Rooftree::Server::GetRequestPath(context.request);
        if (!path)
        {
            context.response.result(http::status::bad_request);
            return RequestHandlerResult::Handled;
        }

        // The API's own 404, in its JSON
        if (path->starts_with("/api/"))
        {
            Rooftree::Server::SetJsonError(context.response, http::status::not_found, "not_found");
            return RequestHandlerResult::Handled;
        }

        if (context.request.method() != verb::get && context.request.method() != verb::head)
        {
            context.response.result(http::status::method_not_allowed);
            context.response.set(http::field::allow, "GET, HEAD");
            return RequestHandlerResult::Handled;
        }

        std::shared_ptr<Rooftree::Server::StaticSite const> current = site.Get();
        ServeSite(context, *path, current.get());
        return RequestHandlerResult::Handled;
    });
}
}
