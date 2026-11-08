/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "HttpRequestUtils.h"
#include "Util.h"
#include <boost/url/parse.hpp>
#include <boost/url/parse_query.hpp>
#include <boost/url/url_view.hpp>
#include <algorithm>

namespace Rooftree::Net::Http
{
namespace
{
constexpr std::string_view Whitespace = " \t";

std::string_view Trim(std::string_view text)
{
    std::size_t begin = text.find_first_not_of(Whitespace);
    if (begin == std::string_view::npos)
        return {};

    return text.substr(begin, text.find_last_not_of(Whitespace) - begin + 1);
}

bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && std::ranges::equal(left, right, [](char a, char b)
    {
        return Rooftree::CharToLower(a) == Rooftree::CharToLower(b);
    });
}

boost::urls::encoding_opts FormEncoding()
{
    boost::urls::encoding_opts options;
    options.space_as_plus = true;
    return options;
}

NameValuePairs DecodeParams(boost::urls::params_encoded_view params)
{
    NameValuePairs pairs;
    pairs.reserve(params.size());
    for (boost::urls::param_pct_view param : params)
        pairs.emplace_back(param.key.decode(FormEncoding()), param.has_value ? param.value.decode(FormEncoding()) : std::string());

    return pairs;
}

// Parameters of a header value after its first token: `form-data; name="a"; filename="b.png"`.
// Names are lower-cased; quoted strings are unquoted with their \-escapes. Empty if malformed
Optional<NameValuePairs> ParseHeaderParameters(std::string_view value)
{
    NameValuePairs parameters;
    std::size_t semicolon = value.find(';');
    if (semicolon == std::string_view::npos)
        return parameters;

    std::string_view rest = value.substr(semicolon + 1);
    while (!(rest = Trim(rest)).empty())
    {
        std::size_t equals = rest.find('=');
        if (equals == std::string_view::npos)
            return {};

        std::string name(Trim(rest.substr(0, equals)));
        std::ranges::transform(name, name.begin(), Rooftree::CharToLower);
        rest = Trim(rest.substr(equals + 1));

        std::string parameterValue;
        if (rest.starts_with('"'))
        {
            std::size_t i = 1;
            for (; i < rest.size() && rest[i] != '"'; ++i)
            {
                if (rest[i] == '\\' && i + 1 < rest.size())
                    ++i;
                parameterValue += rest[i];
            }

            if (i >= rest.size())
                return {};

            rest.remove_prefix(i + 1);
            rest = Trim(rest);
            if (!rest.empty() && !rest.starts_with(';'))
                return {};
        }
        else
        {
            std::size_t end = rest.find(';');
            parameterValue = Trim(rest.substr(0, end));
            rest = end == std::string_view::npos ? std::string_view() : rest.substr(end);
        }

        parameters.emplace_back(std::move(name), std::move(parameterValue));
        if (rest.starts_with(';'))
            rest.remove_prefix(1);
    }

    return parameters;
}

Optional<std::string_view> FindParameter(NameValuePairs const& parameters, std::string_view name)
{
    auto itr = std::ranges::find(parameters, name, &NameValuePairs::value_type::first);
    if (itr == parameters.end())
        return {};

    return std::string_view(itr->second);
}

// First token of a header value, `multipart/form-data; boundary=x` -> `multipart/form-data`
std::string_view GetHeaderToken(std::string_view value)
{
    return Trim(value.substr(0, value.find(';')));
}
}

std::string_view GetTargetPath(std::string_view target)
{
    return target.substr(0, target.find('?'));
}

Optional<std::vector<std::string>> DecodePathSegments(std::string_view target)
{
    boost::system::result<boost::urls::url_view> url = boost::urls::parse_origin_form(target);
    if (!url)
        return {};

    std::vector<std::string> segments;
    segments.reserve(url->encoded_segments().size());
    for (boost::urls::pct_string_view segment : url->encoded_segments())
    {
        std::string decoded = segment.decode();
        if (decoded.find_first_of(std::string_view("/\0", 2)) != std::string::npos)
            return {};

        segments.push_back(std::move(decoded));
    }

    return segments;
}

NameValuePairs GetQueryParameters(std::string_view target)
{
    boost::system::result<boost::urls::url_view> url = boost::urls::parse_origin_form(target);
    if (!url || !url->has_query())
        return {};

    return DecodeParams(url->encoded_params());
}

Optional<std::string> GetQueryParameter(std::string_view target, std::string_view name)
{
    for (auto& [key, value] : GetQueryParameters(target))
        if (key == name)
            return std::move(value);

    return {};
}

Optional<NameValuePairs> ParseUrlEncodedForm(std::string_view body)
{
    boost::system::result<boost::urls::params_encoded_view> params = boost::urls::parse_query(body);
    if (!params)
        return {};

    if (body.empty())
        return NameValuePairs();

    return DecodeParams(*params);
}

Optional<std::string_view> GetCookie(Request const& request, std::string_view name)
{
    auto [begin, end] = request.equal_range(boost::beast::http::field::cookie);
    for (auto header = begin; header != end; ++header)
    {
        std::string_view cookies = ToStdStringView(header->value());
        while (!cookies.empty())
        {
            std::size_t separator = cookies.find(';');
            std::string_view cookie = Trim(cookies.substr(0, separator));
            cookies = separator == std::string_view::npos ? std::string_view() : cookies.substr(separator + 1);

            std::size_t equals = cookie.find('=');
            if (equals == std::string_view::npos || Trim(cookie.substr(0, equals)) != name)
                continue;

            std::string_view value = Trim(cookie.substr(equals + 1));
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);

            return value;
        }
    }

    return {};
}

std::string SerializeSetCookie(Cookie const& cookie)
{
    std::string header = cookie.Name;
    header += '=';
    header += cookie.Value;

    if (!cookie.Path.empty())
    {
        header += "; Path=";
        header += cookie.Path;
    }

    if (cookie.MaxAge)
    {
        header += "; Max-Age=";
        header += std::to_string(cookie.MaxAge->count());
        if (cookie.MaxAge->count() <= 0)
            header += "; Expires=Thu, 01 Jan 1970 00:00:00 GMT";
    }

    if (cookie.HttpOnly)
        header += "; HttpOnly";

    // Browsers refuse SameSite=None without Secure
    if (cookie.Secure || cookie.SameSitePolicy == SameSite::None)
        header += "; Secure";

    switch (cookie.SameSitePolicy)
    {
        case SameSite::Strict: header += "; SameSite=Strict"; break;
        case SameSite::Lax: header += "; SameSite=Lax"; break;
        case SameSite::None: header += "; SameSite=None"; break;
    }

    return header;
}

void SetCookie(Response& response, Cookie const& cookie)
{
    response.insert(boost::beast::http::field::set_cookie, SerializeSetCookie(cookie));
}

Optional<std::vector<MultipartPart>> ParseMultipartFormData(std::string_view contentType, std::string_view body)
{
    if (!EqualsIgnoreCase(GetHeaderToken(contentType), "multipart/form-data"))
        return {};

    Optional<NameValuePairs> contentTypeParameters = ParseHeaderParameters(contentType);
    Optional<std::string_view> boundary = contentTypeParameters ? FindParameter(*contentTypeParameters, "boundary") : Optional<std::string_view>();
    // RFC 2046: 1 to 70 characters
    if (!boundary || boundary->empty() || boundary->size() > 70)
        return {};

    std::string delimiter = "--";
    delimiter += *boundary;

    // The first delimiter may follow a preamble, the next ones always follow a line break
    std::size_t position = body.starts_with(delimiter) ? 0 : body.find("\r\n" + delimiter);
    if (position == std::string_view::npos)
        return {};
    position += body.starts_with(delimiter) ? delimiter.size() : delimiter.size() + 2;

    std::string const nextDelimiter = "\r\n" + delimiter;
    std::vector<MultipartPart> parts;
    while (true)
    {
        std::string_view rest = body.substr(position);
        if (rest.starts_with("--"))
            return parts;

        // Transport padding is allowed after the delimiter
        std::size_t lineEnd = rest.find("\r\n");
        if (lineEnd == std::string_view::npos || !Trim(rest.substr(0, lineEnd)).empty())
            return {};
        rest.remove_prefix(lineEnd + 2);

        std::size_t headersEnd = rest.find("\r\n\r\n");
        // A part without headers starts with the blank line right away
        std::string_view headers;
        if (rest.starts_with("\r\n"))
            rest.remove_prefix(2);
        else if (headersEnd != std::string_view::npos)
        {
            headers = rest.substr(0, headersEnd);
            rest.remove_prefix(headersEnd + 4);
        }
        else
            return {};

        std::size_t dataEnd = rest.find(nextDelimiter);
        if (dataEnd == std::string_view::npos)
            return {};

        MultipartPart part;
        part.Data = rest.substr(0, dataEnd);
        bool hasDisposition = false;
        while (!headers.empty())
        {
            std::size_t end = headers.find("\r\n");
            std::string_view header = headers.substr(0, end);
            headers = end == std::string_view::npos ? std::string_view() : headers.substr(end + 2);

            std::size_t colon = header.find(':');
            if (colon == std::string_view::npos)
                return {};

            std::string_view headerName = Trim(header.substr(0, colon));
            std::string_view headerValue = Trim(header.substr(colon + 1));
            if (EqualsIgnoreCase(headerName, "Content-Disposition"))
            {
                Optional<NameValuePairs> parameters = ParseHeaderParameters(headerValue);
                if (!EqualsIgnoreCase(GetHeaderToken(headerValue), "form-data") || !parameters)
                    return {};

                Optional<std::string_view> name = FindParameter(*parameters, "name");
                if (!name)
                    return {};

                part.Name = *name;
                if (Optional<std::string_view> fileName = FindParameter(*parameters, "filename"))
                    part.FileName.emplace(*fileName);
                hasDisposition = true;
            }
            else if (EqualsIgnoreCase(headerName, "Content-Type"))
                part.ContentType = headerValue;
        }

        if (!hasDisposition)
            return {};

        parts.push_back(std::move(part));
        position = std::size_t(rest.data() - body.data()) + dataEnd + nextDelimiter.size();
    }
}
}
