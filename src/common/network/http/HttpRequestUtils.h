/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What a JSON API needs from a request beyond Beast's message, all returning plain std
 * types so callers don't depend on Boost.URL:
 *
 *  - the path split into percent-decoded segments, the query string and urlencoded forms as name/value
 *    pairs (Boost.URL does the decoding)
 *  - cookies: reading the Cookie header, writing Set-Cookie
 *  - multipart/form-data bodies, for file uploads. Beast reads the whole body into the request already
 *    (bounded by the parser's body limit), so the parts are views into it, nothing is copied
 */

#ifndef ROOFTREE_HTTP_REQUEST_UTILS_H
#define ROOFTREE_HTTP_REQUEST_UTILS_H

#include "Define.h"
#include "Duration.h"
#include "HttpCommon.h"
#include "Optional.h"
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Rooftree::Net::Http
{
using NameValuePairs = std::vector<std::pair<std::string, std::string>>;

// Path of an origin-form target ("/a/b?c=d" -> "/a/b"), still percent-encoded
std::string_view GetTargetPath(std::string_view target);

// "/api/posts/a%20b" -> {"api", "posts", "a b"}; "/" -> {}. Empty if the target is not a valid origin-form
// target or a segment decodes to something containing '/' or a NUL, which no route should ever see
Optional<std::vector<std::string>> DecodePathSegments(std::string_view target);

// Every parameter of the target's query, decoded ('+' is a space, like in forms). Empty for no query
// or a malformed one
NameValuePairs GetQueryParameters(std::string_view target);

// First value of a query parameter
Optional<std::string> GetQueryParameter(std::string_view target, std::string_view name);

// An application/x-www-form-urlencoded body. Empty if malformed
Optional<NameValuePairs> ParseUrlEncodedForm(std::string_view body);

// Value of a cookie of the request's Cookie header(s); the first one if it is sent twice
Optional<std::string_view> GetCookie(Request const& request, std::string_view name);

enum class SameSite
{
    Strict,
    Lax,
    None
};

// Defaults suit a session cookie of a site served over HTTPS
struct Cookie
{
    std::string Name;
    std::string Value;              // sent as is: use only cookie-octets (base64url, hex, ...)
    std::string Path = "/";
    Optional<Seconds> MaxAge;       // none: a browser session cookie; 0: deletes the cookie
    bool HttpOnly = true;
    bool Secure = true;
    SameSite SameSitePolicy = SameSite::Lax;
};

std::string SerializeSetCookie(Cookie const& cookie);

// Adds a Set-Cookie header, a response may carry several
void SetCookie(Response& response, Cookie const& cookie);

struct MultipartPart
{
    std::string Name;               // of Content-Disposition: form-data; name="..."
    Optional<std::string> FileName; // only for file inputs
    std::string ContentType = "text/plain";
    std::string_view Data;          // into the parsed body
};

// A multipart/form-data body with the request's Content-Type value (which holds the boundary).
// Empty if the content type is not multipart/form-data or the body is malformed
Optional<std::vector<MultipartPart>> ParseMultipartFormData(std::string_view contentType, std::string_view body);
}

#endif // ROOFTREE_HTTP_REQUEST_UTILS_H
