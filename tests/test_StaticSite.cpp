/*
 * StaticSite: a directory read into memory, the paths of request targets, and the responses (ETag and
 * 304, the gzip body to clients that take it).
 */

#include "rooftree_catch2.h"

#include "StaticSite.h"
#include <filesystem>
#include <fstream>
#include <random>

using namespace Rooftree::Server;
namespace Net = Rooftree::Net;
namespace http = boost::beast::http;

namespace
{
// A fresh directory under the system's temp directory, removed with the object
class TempDirectory
{
public:
    TempDirectory()
    {
        std::random_device random;
        _path = std::filesystem::temp_directory_path() / ("rooftree-static-" + std::to_string(random()));
        std::filesystem::create_directories(_path);
    }

    ~TempDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(_path, error);
    }

    std::filesystem::path const& Path() const { return _path; }

    void Write(std::string const& name, std::string const& content) const
    {
        std::filesystem::path path = _path / name;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary) << content;
    }

private:
    std::filesystem::path _path;
};

Net::Http::RequestContext Get(std::string_view target, std::initializer_list<std::pair<http::field, std::string_view>> headers = {})
{
    Net::Http::RequestContext context;
    context.request.method(http::verb::get);
    context.request.target(target);
    for (auto const& [field, value] : headers)
        context.request.set(field, value);
    return context;
}
}

TEST_CASE("StaticSite loads a directory into memory", "[server][static]")
{
    TempDirectory directory;
    directory.Write("index.html", "<h1>home</h1>");
    directory.Write("zh/index.html", "<h1>zh</h1>");
    directory.Write("assets/app-1a2b.js", "console.log(1)");
    directory.Write("assets/app-1a2b.js.gz", "gzipped bytes");
    directory.Write("orphan.txt.gz", "no plain twin");

    std::string error;
    std::shared_ptr<StaticSite const> site = StaticSite::Load(directory.Path(), error);
    REQUIRE(site);
    CHECK(error.empty());
    CHECK(site->GetFileCount() == 4);

    StaticFile const* home = site->Find("/index.html");
    REQUIRE(home);
    CHECK(home->Body == "<h1>home</h1>");
    CHECK(home->ContentType == "text/html; charset=utf-8");
    CHECK(home->GzipBody.empty());
    CHECK(home->ETag.size() == 34);
    CHECK(home->ETag.front() == '"');

    StaticFile const* script = site->Find("/assets/app-1a2b.js");
    REQUIRE(script);
    CHECK(script->GzipBody == "gzipped bytes");
    CHECK(script->ContentType == "text/javascript; charset=utf-8");
    // The .gz belongs to its file, it isn't one of its own
    CHECK_FALSE(site->Find("/assets/app-1a2b.js.gz"));
    // Unless there is no such file
    CHECK(site->Find("/orphan.txt.gz"));

    CHECK(site->Find("/zh/index.html")->Body == "<h1>zh</h1>");
    CHECK_FALSE(site->Find("/zh"));
    CHECK_FALSE(site->Find("/missing.html"));

    SECTION("through a symlink, like a release behind current")
    {
        std::filesystem::path link = directory.Path().parent_path() / (directory.Path().filename().string() + "-current");
        std::filesystem::create_directory_symlink(directory.Path(), link);
        std::shared_ptr<StaticSite const> linked = StaticSite::Load(link, error);
        std::filesystem::remove(link);
        REQUIRE(linked);
        CHECK(linked->Find("/zh/index.html"));
    }

    SECTION("a missing directory")
    {
        CHECK_FALSE(StaticSite::Load(directory.Path() / "nope", error));
        CHECK_FALSE(error.empty());
    }

    SECTION("the holder swaps sites as a whole")
    {
        StaticSiteHolder holder;
        CHECK_FALSE(holder.Get());
        holder.Set(site);
        std::shared_ptr<StaticSite const> current = holder.Get();
        holder.Set(nullptr);
        // Who still has the old one keeps it
        CHECK(current->Find("/index.html"));
        CHECK_FALSE(holder.Get());
    }
}

TEST_CASE("Request paths and content types", "[server][static]")
{
    auto path = [](std::string_view target) { return GetRequestPath(Get(target).request); };

    CHECK(path("/") == "/");
    CHECK(path("/zh") == "/zh");
    CHECK(path("/zh/") == "/zh/");
    CHECK(path("/zh/posts/a%20b?x=1") == "/zh/posts/a b");
    CHECK(path("//zh//about/") == "/zh/about/");
    // Refused like routes refuse them: an encoded slash, not origin-form
    CHECK_FALSE(path("/a%2Fb"));
    CHECK_FALSE(path("http://example.com/"));

    CHECK(GetContentType("/a/b.css") == "text/css; charset=utf-8");
    CHECK(GetContentType("/fonts/x.woff2") == "font/woff2");
    CHECK(GetContentType("/zh.data") == "application/octet-stream");
    CHECK(GetContentType("/noextension") == "application/octet-stream");
}

TEST_CASE("SetStaticResponse: ETag, 304 and gzip", "[server][static]")
{
    StaticFile file{ .Body = "plain body", .GzipBody = "gzip body", .ContentType = "text/plain; charset=utf-8", .ETag = "\"abc\"" };

    SECTION("plain, without Accept-Encoding")
    {
        Net::Http::RequestContext context = Get("/a.txt");
        SetStaticResponse(context, file, "no-cache");
        CHECK(context.response.result() == http::status::ok);
        CHECK(context.response.body() == "plain body");
        CHECK(context.response[http::field::etag] == "\"abc\"");
        CHECK(context.response[http::field::cache_control] == "no-cache");
        CHECK(context.response[http::field::vary] == "Accept-Encoding");
        CHECK(context.response[http::field::content_encoding].empty());
    }

    SECTION("gzip to a client that takes it, with its own ETag")
    {
        Net::Http::RequestContext context = Get("/a.txt", { { http::field::accept_encoding, "br, gzip;q=0.8" } });
        SetStaticResponse(context, file, "no-cache");
        CHECK(context.response.body() == "gzip body");
        CHECK(context.response[http::field::content_encoding] == "gzip");
        CHECK(context.response[http::field::etag] == "\"abc-gz\"");
    }

    SECTION("q=0 refuses gzip")
    {
        for (std::string_view acceptEncoding : { "gzip;q=0", "gzip; q=0.0", "br" })
        {
            Net::Http::RequestContext context = Get("/a.txt", { { http::field::accept_encoding, acceptEncoding } });
            SetStaticResponse(context, file, "no-cache");
            CHECK(context.response.body() == "plain body");
        }
    }

    SECTION("304 for the tag the client has")
    {
        for (std::string_view ifNoneMatch : { "\"abc\"", "W/\"abc\"", "\"x\", \"abc\"", "*" })
        {
            Net::Http::RequestContext context = Get("/a.txt", { { http::field::if_none_match, ifNoneMatch } });
            SetStaticResponse(context, file, "no-cache");
            CHECK(context.response.result() == http::status::not_modified);
            CHECK(context.response.body().empty());
        }

        Net::Http::RequestContext other = Get("/a.txt", { { http::field::if_none_match, "\"abc-gz\"" } });
        SetStaticResponse(other, file, "no-cache");
        CHECK(other.response.result() == http::status::ok);
    }

    SECTION("a 404 page is never a 304")
    {
        Net::Http::RequestContext context = Get("/missing", { { http::field::if_none_match, "\"abc\"" } });
        SetStaticResponse(context, file, "no-cache", http::status::not_found);
        CHECK(context.response.result() == http::status::not_found);
        CHECK(context.response.body() == "plain body");
    }
}
