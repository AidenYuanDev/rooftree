/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Outgoing HTTP(S) requests with libcurl, e.g. to an OAuth provider's token and user endpoints.
 * HttpFetch blocks until the whole response is in, so it runs on a worker (Session::OnWorker),
 * never on the main thread.
 *
 * Only http:// and https:// URLs, no redirects followed, certificates checked, and a response body
 * larger than the request's MaxResponseSize is an error rather than kept.
 */

#ifndef ROOFTREE_HTTP_CLIENT_H
#define ROOFTREE_HTTP_CLIENT_H

#include "Define.h"
#include "Duration.h"
#include <string>
#include <utility>
#include <vector>

namespace Rooftree::Server
{
struct HttpClientRequest
{
    std::string Method = "GET";
    std::string Url;
    std::vector<std::pair<std::string, std::string>> Headers;
    std::string Body;
    Seconds Timeout = 15s;
    std::size_t MaxResponseSize = 1024 * 1024;      // a larger body is an error
};

struct HttpClientResponse
{
    uint32 Status = 0;          // 0 when no response came: Error says why
    std::string ContentType;
    std::string Body;
    std::string Error;
};

HttpClientResponse HttpFetch(HttpClientRequest const& request);

// application/x-www-form-urlencoded: "a=1&b=x%20y"
std::string FormUrlEncode(std::vector<std::pair<std::string, std::string>> const& fields);
}

#endif // ROOFTREE_HTTP_CLIENT_H
