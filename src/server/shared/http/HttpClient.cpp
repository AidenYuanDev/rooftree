/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "HttpClient.h"
#include <curl/curl.h>
#include <memory>
#include <mutex>

namespace Rooftree::Server
{
namespace
{
struct Download
{
    std::string Body;
    std::size_t MaxSize = 0;
    bool TooLarge = false;
};

std::size_t WriteBody(char* data, std::size_t size, std::size_t count, void* userData)
{
    Download& download = *static_cast<Download*>(userData);
    std::size_t bytes = size * count;
    if (download.Body.size() + bytes > download.MaxSize)
    {
        download.TooLarge = true;
        return 0; // makes curl stop with CURLE_WRITE_ERROR
    }

    download.Body.append(data, bytes);
    return bytes;
}
}

HttpClientResponse HttpFetch(HttpClientRequest const& request)
{
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

    HttpClientResponse response;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl)
    {
        response.Error = "curl_easy_init failed";
        return response;
    }

    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, &curl_slist_free_all);
    bool hasUserAgent = false;
    for (auto const& [name, value] : request.Headers)
    {
        std::string line = name + ": " + value;
        if (line.find_first_of("\r\n") != std::string::npos)
        {
            response.Error = "a header with a line break";
            return response;
        }

        headers.reset(curl_slist_append(headers.release(), line.c_str()));
        hasUserAgent |= name.size() == 10 && curl_strnequal(name.c_str(), "User-Agent", 10);
    }
    // Some APIs (GitHub's) refuse requests without one
    if (!hasUserAgent)
        headers.reset(curl_slist_append(headers.release(), "User-Agent: Rooftree"));

    Download download{ .MaxSize = request.MaxResponseSize };
    char errorBuffer[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl.get(), CURLOPT_URL, request.Url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl.get(), CURLOPT_CUSTOMREQUEST, request.Method.c_str());
    if (!request.Body.empty() || request.Method == "POST")
    {
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request.Body.data());
        curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE_LARGE, curl_off_t(request.Body.size()));
    }
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, &WriteBody);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, long(request.Timeout.count()));
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, errorBuffer);

    CURLcode result = curl_easy_perform(curl.get());
    if (download.TooLarge)
    {
        response.Error = "the response is too large";
        return response;
    }
    if (result != CURLE_OK)
    {
        response.Error = errorBuffer[0] ? errorBuffer : curl_easy_strerror(result);
        return response;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    response.Status = uint32(status);
    char const* contentType = nullptr;
    if (curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &contentType) == CURLE_OK && contentType)
        response.ContentType = contentType;
    response.Body = std::move(download.Body);
    return response;
}

std::string FormUrlEncode(std::vector<std::pair<std::string, std::string>> const& fields)
{
    auto encode = [](std::string_view text)
    {
        static constexpr char Hex[] = "0123456789ABCDEF";
        std::string encoded;
        for (unsigned char c : text)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~')
                encoded += char(c);
            else
            {
                encoded += '%';
                encoded += Hex[c >> 4];
                encoded += Hex[c & 15];
            }
        }
        return encoded;
    };

    std::string body;
    for (auto const& [name, value] : fields)
    {
        if (!body.empty())
            body += '&';
        body += encode(name) + '=' + encode(value);
    }
    return body;
}
}
