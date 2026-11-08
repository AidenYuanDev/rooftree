/*
 * The request-side helpers of rooftree-network's HTTP module without sockets: route matching, path, query
 * and form decoding, cookies and multipart bodies.
 */

#include "rooftree_catch2.h"

#include "HttpRequestUtils.h"
#include "HttpRouter.h"

using namespace Rooftree::Net::Http;
using boost::beast::http::verb;

namespace
{
std::vector<std::string> Segments(std::string_view target)
{
    Optional<std::vector<std::string>> segments = DecodePathSegments(target);
    REQUIRE(segments.has_value());
    return *segments;
}
}

TEST_CASE("Router matches literal and parameter segments", "[http][router]")
{
    Router<int> router;
    REQUIRE(router.Add(verb::get, "/", 1));
    REQUIRE(router.Add(verb::get, "/posts/{id}", 2));
    REQUIRE(router.Add(verb::get, "/posts/slugs", 3));
    REQUIRE(router.Add(verb::delete_, "/posts/{id}", 4));
    REQUIRE(router.Add(verb::get, "/posts/{id}/comments/{commentId}", 5));
    REQUIRE(router.Add(verb::get, "/posts/slugs/{lang}", 6));

    int const* handler = nullptr;
    PathParameters parameters;

    CHECK(router.Find(verb::get, Segments("/"), handler, parameters) == RouteMatchResult::Found);
    CHECK(*handler == 1);

    CHECK(router.Find(verb::get, Segments("/posts/hello%20world"), handler, parameters) == RouteMatchResult::Found);
    CHECK(*handler == 2);
    CHECK(parameters.Get("id") == "hello world");

    SECTION("literal segments win whatever the order")
    {
        CHECK(router.Find(verb::get, Segments("/posts/slugs"), handler, parameters) == RouteMatchResult::Found);
        CHECK(*handler == 3);
        CHECK(parameters.GetAll().empty());
    }

    SECTION("the parameter branch is tried when the literal one has no route for the rest")
    {
        CHECK(router.Find(verb::get, Segments("/posts/slugs/comments/7"), handler, parameters) == RouteMatchResult::Found);
        CHECK(*handler == 5);
        CHECK(parameters.Get("id") == "slugs");
        CHECK(parameters.Get("commentId") == "7");
        REQUIRE(parameters.GetAll().size() == 2);
        CHECK(parameters.GetAll()[0].first == "id");
    }

    SECTION("methods")
    {
        CHECK(router.Find(verb::delete_, Segments("/posts/3"), handler, parameters) == RouteMatchResult::Found);
        CHECK(*handler == 4);

        // HEAD falls back to GET
        CHECK(router.Find(verb::head, Segments("/posts/3"), handler, parameters) == RouteMatchResult::Found);
        CHECK(*handler == 2);

        std::string allowed;
        CHECK(router.Find(verb::put, Segments("/posts/3"), handler, parameters, &allowed) == RouteMatchResult::MethodNotAllowed);
        CHECK(allowed == "DELETE, GET, HEAD");
        CHECK(parameters.GetAll().empty());
    }

    SECTION("no match")
    {
        CHECK(router.Find(verb::get, Segments("/posts"), handler, parameters) == RouteMatchResult::NotFound);
        CHECK(router.Find(verb::get, Segments("/posts/"), handler, parameters) == RouteMatchResult::NotFound);  // empty {id}
        CHECK(router.Find(verb::get, Segments("/posts/1/2"), handler, parameters) == RouteMatchResult::NotFound);
        CHECK(router.Find(verb::get, Segments("/other"), handler, parameters) == RouteMatchResult::NotFound);
    }

    SECTION("invalid registrations")
    {
        CHECK_FALSE(router.Add(verb::get, "/posts/{id}", 9));            // duplicate
        CHECK_FALSE(router.Add(verb::get, "/posts/{slug}/x", 9));        // other name at the same place
        CHECK_FALSE(router.Add(verb::get, "/a/{x}/{x}", 9));
        CHECK_FALSE(router.Add(verb::get, "/a/{}", 9));
        CHECK_FALSE(router.Add(verb::get, "/a/{b", 9));
        CHECK_FALSE(router.Add(verb::get, "/a/b}", 9));
        CHECK_FALSE(router.Add(verb::get, "relative", 9));
        CHECK(router.Add(verb::post, "/posts/{id}", 9));
    }
}

TEST_CASE("Request paths, queries and forms are decoded", "[http]")
{
    CHECK(Segments("/").empty());
    CHECK(Segments("/a/b%2Bc/d?x=1") == std::vector<std::string>{ "a", "b+c", "d" });
    CHECK(Segments("/a/") == std::vector<std::string>{ "a", "" });
    CHECK(Segments("/%E4%B8%AD") == std::vector<std::string>{ "\xE4\xB8\xAD" });

    // Decoded '/' or NUL would let a segment pose as two, or end a C string early
    CHECK_FALSE(DecodePathSegments("/a%2Fb").has_value());
    CHECK_FALSE(DecodePathSegments("/a%00").has_value());
    CHECK_FALSE(DecodePathSegments("no-slash").has_value());
    CHECK_FALSE(DecodePathSegments("/bad%zz").has_value());

    CHECK(GetTargetPath("/a/b?c=d") == "/a/b");

    NameValuePairs query = GetQueryParameters("/search?q=hello+world&tag=c%2B%2B&flag&q=second");
    REQUIRE(query.size() == 4);
    CHECK(query[0] == std::pair<std::string, std::string>("q", "hello world"));
    CHECK(query[1] == std::pair<std::string, std::string>("tag", "c++"));
    CHECK(query[2] == std::pair<std::string, std::string>("flag", ""));
    CHECK(GetQueryParameter("/search?q=hello+world&q=second", "q") == "hello world");
    CHECK_FALSE(GetQueryParameter("/search", "q"));
    CHECK(GetQueryParameters("/search").empty());

    Optional<NameValuePairs> form = ParseUrlEncodedForm("name=%E4%B8%AD+x&empty=");
    REQUIRE(form.has_value());
    REQUIRE(form->size() == 2);
    CHECK((*form)[0].second == "\xE4\xB8\xAD x");
    CHECK((*form)[1].second.empty());
    CHECK(ParseUrlEncodedForm("")->empty());
    CHECK_FALSE(ParseUrlEncodedForm("a=%zz").has_value());
}

TEST_CASE("Cookies are read and written", "[http]")
{
    Request request;
    request.insert(boost::beast::http::field::cookie, "theme=dark; sid=abc123");
    request.insert(boost::beast::http::field::cookie, "lang=\"zh\"; sid=second");

    CHECK(GetCookie(request, "sid") == "abc123");
    CHECK(GetCookie(request, "theme") == "dark");
    CHECK(GetCookie(request, "lang") == "zh");
    CHECK_FALSE(GetCookie(request, "missing"));
    CHECK_FALSE(GetCookie(request, "the"));

    Cookie session{ .Name = "sid", .Value = "abc", .MaxAge = Seconds(3600) };
    CHECK(SerializeSetCookie(session) == "sid=abc; Path=/; Max-Age=3600; HttpOnly; Secure; SameSite=Lax");

    Cookie removed{ .Name = "sid", .Value = "", .MaxAge = Seconds(0), .HttpOnly = false, .Secure = false, .SameSitePolicy = SameSite::Strict };
    CHECK(SerializeSetCookie(removed) == "sid=; Path=/; Max-Age=0; Expires=Thu, 01 Jan 1970 00:00:00 GMT; SameSite=Strict");

    Response response;
    SetCookie(response, session);
    SetCookie(response, removed);
    CHECK(std::distance(response.equal_range(boost::beast::http::field::set_cookie).first,
        response.equal_range(boost::beast::http::field::set_cookie).second) == 2);
}

TEST_CASE("multipart/form-data bodies are split into parts", "[http]")
{
    std::string_view contentType = "multipart/form-data; boundary=\"----x\"";
    std::string body =
        "preamble\r\n"
        "------x\r\n"
        "Content-Disposition: form-data; name=\"title\"\r\n"
        "\r\n"
        "Hello\r\n"
        "------x\r\n"
        "content-disposition: form-data; name=\"file\"; filename=\"a \\\"b\\\".png\"\r\n"
        "Content-Type: image/png\r\n"
        "\r\n";
    body += std::string("\x89PNG\r\n\0\r\n--", 11);
    body +=
        "\r\n"
        "------x--\r\n"
        "epilogue";

    Optional<std::vector<MultipartPart>> parts = ParseMultipartFormData(contentType, body);
    REQUIRE(parts.has_value());
    REQUIRE(parts->size() == 2);
    CHECK((*parts)[0].Name == "title");
    CHECK_FALSE((*parts)[0].FileName);
    CHECK((*parts)[0].ContentType == "text/plain");
    CHECK((*parts)[0].Data == "Hello");
    CHECK((*parts)[1].Name == "file");
    CHECK((*parts)[1].FileName == "a \"b\".png");
    CHECK((*parts)[1].ContentType == "image/png");
    CHECK((*parts)[1].Data == std::string_view("\x89PNG\r\n\0\r\n--", 11));

    CHECK(ParseMultipartFormData("multipart/form-data; boundary=b", "--b--\r\n")->empty());

    CHECK_FALSE(ParseMultipartFormData("application/json", body).has_value());
    CHECK_FALSE(ParseMultipartFormData("multipart/form-data", body).has_value());                         // no boundary
    CHECK_FALSE(ParseMultipartFormData("multipart/form-data; boundary=b", "--b\r\n\r\nx").has_value());   // not terminated
    CHECK_FALSE(ParseMultipartFormData("multipart/form-data; boundary=b",
        "--b\r\nContent-Type: text/plain\r\n\r\nx\r\n--b--").has_value());                                 // no name
}
