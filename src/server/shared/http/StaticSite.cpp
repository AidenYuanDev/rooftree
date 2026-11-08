/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "StaticSite.h"
#include "HttpRequestUtils.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <fstream>

namespace http = boost::beast::http;

namespace Rooftree::Server
{
namespace
{
constexpr std::string_view GzipSuffix = ".gz";

bool EndsWith(std::string_view text, std::string_view suffix)
{
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

// "gzip", "gzip;q=0.5", "*" in a comma separated list, but not with q=0
bool AcceptsGzip(std::string_view acceptEncoding)
{
    while (!acceptEncoding.empty())
    {
        std::size_t comma = acceptEncoding.find(',');
        std::string_view item = acceptEncoding.substr(0, comma);
        acceptEncoding = comma == std::string_view::npos ? std::string_view() : acceptEncoding.substr(comma + 1);

        std::size_t semicolon = item.find(';');
        std::string_view coding = item.substr(0, semicolon);
        std::string_view parameters = semicolon == std::string_view::npos ? std::string_view() : item.substr(semicolon + 1);
        while (!coding.empty() && coding.front() == ' ')
            coding.remove_prefix(1);
        while (!coding.empty() && coding.back() == ' ')
            coding.remove_suffix(1);

        if (coding != "gzip" && coding != "*")
            continue;

        std::size_t q = parameters.find("q=");
        if (q == std::string_view::npos)
            return true;
        // q=0, q=0.0, q=0.000: refused; any other weight is above 0
        std::string_view value = parameters.substr(q + 2);
        value = value.substr(0, value.find_first_of(" ;"));
        if (!value.empty() && value.find_first_not_of("0.") == std::string_view::npos)
            continue;
        return true;
    }
    return false;
}

// If-None-Match: "*" or a list of (possibly weak) tags
bool MatchesETag(std::string_view ifNoneMatch, std::string_view etag)
{
    if (ifNoneMatch.empty())
        return false;

    while (!ifNoneMatch.empty())
    {
        std::size_t comma = ifNoneMatch.find(',');
        std::string_view tag = ifNoneMatch.substr(0, comma);
        ifNoneMatch = comma == std::string_view::npos ? std::string_view() : ifNoneMatch.substr(comma + 1);
        while (!tag.empty() && tag.front() == ' ')
            tag.remove_prefix(1);
        while (!tag.empty() && tag.back() == ' ')
            tag.remove_suffix(1);
        if (tag.starts_with("W/"))
            tag.remove_prefix(2);
        if (tag == "*" || tag == etag)
            return true;
    }
    return false;
}

// The gzip body is another representation, with its own tag: "abc" -> "abc-gz"
std::string GzipETag(std::string_view etag)
{
    std::string tag(etag.substr(0, etag.size() - 1));
    tag += "-gz\"";
    return tag;
}
}

std::shared_ptr<StaticSite const> StaticSite::Load(std::filesystem::path const& directory, std::string& error)
{
    std::error_code ec;
    std::filesystem::path root = std::filesystem::canonical(directory, ec);
    if (ec || !std::filesystem::is_directory(root, ec))
    {
        error = StringFormat("{} is not a directory{}", directory.string(), ec ? ": " + ec.message() : "");
        return nullptr;
    }

    std::shared_ptr<StaticSite> site = std::make_shared<StaticSite>();
    std::unordered_map<std::string, std::string> gzipBodies;
    for (std::filesystem::recursive_directory_iterator itr(root, ec), end; !ec && itr != end; itr.increment(ec))
    {
        if (!itr->is_regular_file(ec))
            continue;

        std::string path = "/" + std::filesystem::relative(itr->path(), root, ec).generic_string();
        Optional<std::string> body = ReadWholeFile(itr->path());
        if (ec || !body)
        {
            error = StringFormat("{} can't be read", itr->path().string());
            return nullptr;
        }

        site->_size += body->size();
        if (EndsWith(path, GzipSuffix))
        {
            gzipBodies.emplace(path.substr(0, path.size() - GzipSuffix.size()), std::move(*body));
            continue;
        }

        StaticFile& file = site->_files[path];
        file.ContentType = GetContentType(path);
        file.ETag = "\"" + Sha256Hex(*body).substr(0, 32) + "\"";
        file.Body = std::move(*body);
    }

    if (ec)
    {
        error = StringFormat("{} can't be read: {}", directory.string(), ec.message());
        return nullptr;
    }

    // A .gz without its file is served as itself, like any other file
    for (auto& [path, gzipBody] : gzipBodies)
    {
        auto itr = site->_files.find(path);
        if (itr != site->_files.end())
            itr->second.GzipBody = std::move(gzipBody);
        else
        {
            StaticFile& file = site->_files[path + std::string(GzipSuffix)];
            file.ContentType = GetContentType(path + std::string(GzipSuffix));
            file.ETag = "\"" + Sha256Hex(gzipBody).substr(0, 32) + "\"";
            file.Body = std::move(gzipBody);
        }
    }

    return site;
}

StaticFile const* StaticSite::Find(std::string_view path) const
{
    auto itr = _files.find(std::string(path));
    return itr != _files.end() ? &itr->second : nullptr;
}

std::shared_ptr<StaticSite const> StaticSiteHolder::Get() const
{
    std::scoped_lock lock(_mutex);
    return _site;
}

void StaticSiteHolder::Set(std::shared_ptr<StaticSite const> site)
{
    // The old one is destroyed outside the lock, by whoever drops its last reference
    std::shared_ptr<StaticSite const> old;
    {
        std::scoped_lock lock(_mutex);
        old = std::exchange(_site, std::move(site));
    }
}

Optional<std::string> GetRequestPath(Net::Http::Request const& request)
{
    std::string_view target = Net::Http::ToStdStringView(request.target());
    Optional<std::vector<std::string>> segments = Net::Http::DecodePathSegments(target);
    if (!segments)
        return {};

    // "/zh/" is {"zh", ""}, "//a" is {"", "a"}
    std::string path;
    for (std::string const& segment : *segments)
    {
        if (segment.empty())
            continue;
        path += '/';
        path += segment;
    }

    std::string_view rawPath = target.substr(0, target.find_first_of("?#"));
    if (path.empty() || rawPath.ends_with('/'))
        path += '/';
    return path;
}

std::string_view GetContentType(std::string_view path)
{
    struct Type
    {
        std::string_view Extension;
        std::string_view ContentType;
    };

    static constexpr Type Types[] =
    {
        { ".html", "text/html; charset=utf-8" },
        { ".js", "text/javascript; charset=utf-8" },
        { ".mjs", "text/javascript; charset=utf-8" },
        { ".css", "text/css; charset=utf-8" },
        { ".json", "application/json" },
        { ".map", "application/json" },
        { ".webmanifest", "application/manifest+json" },
        { ".txt", "text/plain; charset=utf-8" },
        { ".xml", "application/xml" },
        { ".svg", "image/svg+xml" },
        { ".png", "image/png" },
        { ".jpg", "image/jpeg" },
        { ".jpeg", "image/jpeg" },
        { ".gif", "image/gif" },
        { ".webp", "image/webp" },
        { ".avif", "image/avif" },
        { ".ico", "image/x-icon" },
        { ".woff2", "font/woff2" },
        { ".woff", "font/woff" },
        { ".ttf", "font/ttf" },
        { ".wasm", "application/wasm" },
        { ".gz", "application/gzip" },
    };

    std::string_view name = path.substr(path.rfind('/') + 1);
    for (Type const& type : Types)
        if (EndsWith(name, type.Extension))
            return type.ContentType;
    return "application/octet-stream";
}

void SetStaticResponse(Net::Http::RequestContext& context, StaticFile const& file, std::string_view cacheControl, http::status status)
{
    Net::Http::Response& response = context.response;
    bool gzip = !file.GzipBody.empty() && AcceptsGzip(Net::Http::ToStdStringView(context.request[http::field::accept_encoding]));
    std::string etag = gzip ? GzipETag(file.ETag) : file.ETag;

    response.set(http::field::etag, etag);
    if (!cacheControl.empty())
        response.set(http::field::cache_control, cacheControl);
    if (!file.GzipBody.empty())
        response.set(http::field::vary, "Accept-Encoding");

    // A 404 page is not something the client has
    if (status == http::status::ok && MatchesETag(Net::Http::ToStdStringView(context.request[http::field::if_none_match]), etag))
    {
        response.result(http::status::not_modified);
        response.body().clear();
        return;
    }

    response.result(status);
    response.set(http::field::content_type, file.ContentType);
    if (gzip)
        response.set(http::field::content_encoding, "gzip");
    response.body() = gzip ? file.GzipBody : file.Body;
}

Optional<std::string> ReadWholeFile(std::filesystem::path const& path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};

    std::string data(std::istreambuf_iterator<char>(in), {});
    if (in.bad())
        return {};
    return data;
}
}
