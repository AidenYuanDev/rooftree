/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "OAuth.h"
#include "Config.h"
#include "CryptoHash.h"
#include "HttpClient.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <boost/json/parse.hpp>
#include <boost/json/value.hpp>

namespace Blog
{
namespace
{
OAuthProvider ProviderFromConfig(Config const& config, std::string_view path, OAuthProvider defaults)
{
    std::string prefix(path);
    OAuthProvider provider = std::move(defaults);
    provider.ClientId = config.Get<std::string>(prefix + ".ClientId", "");
    provider.ClientSecret = config.Get<std::string>(prefix + ".ClientSecret", "");
    provider.AuthorizeUrl = config.Get<std::string>(prefix + ".AuthorizeUrl", provider.AuthorizeUrl);
    provider.TokenUrl = config.Get<std::string>(prefix + ".TokenUrl", provider.TokenUrl);
    provider.UserUrl = config.Get<std::string>(prefix + ".UserUrl", provider.UserUrl);
    provider.EmailsUrl = config.Get<std::string>(prefix + ".EmailsUrl", provider.EmailsUrl);
    return provider;
}

Optional<boost::json::object> ParseObject(std::string const& text)
{
    boost::system::error_code error;
    boost::json::value value = boost::json::parse(text, error);
    if (error || !value.is_object())
        return {};
    return std::move(value.as_object());
}

std::string GetString(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    if (value && value->is_string())
        return std::string(value->as_string());
    if (value && value->is_int64())
        return std::to_string(value->as_int64());
    if (value && value->is_uint64())
        return std::to_string(value->as_uint64());
    return {};
}

bool GetBool(boost::json::object const& object, std::string_view key)
{
    boost::json::value const* value = object.if_contains(key);
    return value && value->is_bool() && value->as_bool();
}

// GET url with the token, a JSON body back
Optional<boost::json::value> FetchJson(std::string const& url, std::string const& accessToken, std::string& error)
{
    Rooftree::Server::HttpClientResponse response = Rooftree::Server::HttpFetch({ .Url = url,
        .Headers = { { "Authorization", "Bearer " + accessToken }, { "Accept", "application/json" } } });
    if (response.Status != 200)
    {
        error = response.Status ? Rooftree::StringFormat("{} answered {}", url, response.Status) : response.Error;
        return {};
    }

    boost::system::error_code parseError;
    boost::json::value value = boost::json::parse(response.Body, parseError);
    if (parseError)
    {
        error = url + " didn't answer JSON";
        return {};
    }
    return value;
}
}

std::vector<OAuthProvider> OAuthProvider::FromConfig(Config const& config)
{
    return
    {
        ProviderFromConfig(config, "Blog.OAuth.GitHub", { .Name = "github",
            .AuthorizeUrl = "https://github.com/login/oauth/authorize",
            .TokenUrl = "https://github.com/login/oauth/access_token",
            .UserUrl = "https://api.github.com/user",
            .EmailsUrl = "https://api.github.com/user/emails" }),
        ProviderFromConfig(config, "Blog.OAuth.Google", { .Name = "google",
            .AuthorizeUrl = "https://accounts.google.com/o/oauth2/v2/auth",
            .TokenUrl = "https://oauth2.googleapis.com/token",
            .UserUrl = "https://openidconnect.googleapis.com/v1/userinfo" }),
    };
}

OAuthPkce OAuthPkce::Generate()
{
    OAuthPkce pkce;
    pkce.Verifier = Rooftree::Server::GenerateToken(32);
    Rooftree::Crypto::SHA256::Digest digest = Rooftree::Crypto::SHA256::GetDigestOf(reinterpret_cast<uint8 const*>(pkce.Verifier.data()), pkce.Verifier.size());
    pkce.Challenge = Rooftree::Server::Base64UrlEncode(digest);
    return pkce;
}

std::string BuildOAuthAuthorizeUrl(OAuthProvider const& provider, std::string_view redirectUri, std::string_view state, std::string_view codeChallenge)
{
    std::vector<std::pair<std::string, std::string>> query =
    {
        { "client_id", provider.ClientId },
        { "redirect_uri", std::string(redirectUri) },
        { "response_type", "code" },
        { "scope", provider.Name == "google" ? "openid email profile" : "read:user user:email" },
        { "state", std::string(state) },
        { "code_challenge", std::string(codeChallenge) },
        { "code_challenge_method", "S256" },
    };
    // Google would otherwise log in with whichever account the browser has open, without asking
    if (provider.Name == "google")
        query.emplace_back("prompt", "select_account");

    return provider.AuthorizeUrl + (provider.AuthorizeUrl.find('?') != std::string::npos ? '&' : '?') + Rooftree::Server::FormUrlEncode(query);
}

Optional<OAuthIdentity> WorkerFetchOAuthIdentity(OAuthProvider const& provider, std::string const& redirectUri, std::string const& code,
    std::string const& codeVerifier, std::string& error)
{
    Rooftree::Server::HttpClientResponse tokenResponse = Rooftree::Server::HttpFetch({ .Method = "POST", .Url = provider.TokenUrl,
        .Headers = { { "Accept", "application/json" }, { "Content-Type", "application/x-www-form-urlencoded" } },
        .Body = Rooftree::Server::FormUrlEncode({
            { "grant_type", "authorization_code" },
            { "code", code },
            { "redirect_uri", redirectUri },
            { "client_id", provider.ClientId },
            { "client_secret", provider.ClientSecret },
            { "code_verifier", codeVerifier },
        }) });
    if (tokenResponse.Status != 200)
    {
        error = tokenResponse.Status ? Rooftree::StringFormat("the token endpoint answered {}", tokenResponse.Status) : tokenResponse.Error;
        return {};
    }

    // GitHub answers 200 with {"error": ...} for a bad code
    Optional<boost::json::object> token = ParseObject(tokenResponse.Body);
    std::string accessToken = token ? GetString(*token, "access_token") : "";
    if (accessToken.empty())
    {
        error = token ? "no token: " + GetString(*token, "error") : "the token endpoint didn't answer JSON";
        return {};
    }

    Optional<boost::json::value> userValue = FetchJson(provider.UserUrl, accessToken, error);
    if (!userValue || !userValue->is_object())
    {
        if (error.empty())
            error = "the user isn't an object";
        return {};
    }

    boost::json::object const& user = userValue->as_object();
    OAuthIdentity identity;
    if (provider.Name == "google")
    {
        identity.ProviderUserId = GetString(user, "sub");
        identity.Login = GetString(user, "email");
        identity.Name = GetString(user, "name");
        identity.AvatarUrl = GetString(user, "picture");
        if (GetBool(user, "email_verified"))
            identity.Email = identity.Login;
    }
    else
    {
        identity.ProviderUserId = GetString(user, "id");
        identity.Login = GetString(user, "login");
        identity.Name = GetString(user, "name");
        identity.AvatarUrl = GetString(user, "avatar_url");

        // The primary address if verified, else any verified one
        Optional<boost::json::value> emails = FetchJson(provider.EmailsUrl, accessToken, error);
        if (emails && emails->is_array())
        {
            for (boost::json::value const& entry : emails->as_array())
            {
                if (!entry.is_object() || !GetBool(entry.as_object(), "verified"))
                    continue;
                if (identity.Email.empty() || GetBool(entry.as_object(), "primary"))
                    identity.Email = GetString(entry.as_object(), "email");
            }
        }
        error.clear();
    }

    if (identity.ProviderUserId.empty())
    {
        error = "the user has no id";
        return {};
    }

    // The server downloads it later: an https:// address only, plain http only from the tests' fake
    // provider (its endpoints are plain http too)
    bool plainHttp = provider.UserUrl.starts_with("http://");
    if (identity.AvatarUrl.size() > 512 || !(identity.AvatarUrl.starts_with("https://") || (plainHttp && identity.AvatarUrl.starts_with("http://"))))
        identity.AvatarUrl.clear();
    return identity;
}

std::string SuggestUsername(OAuthIdentity const& identity)
{
    std::string_view source = identity.Login;
    // Google's login is an address: the part before @
    if (std::size_t at = source.find('@'); at != std::string_view::npos)
        source = source.substr(0, at);

    std::string name;
    for (char c : source)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
            name += c;
        else if ((c == '-' || c == '.') && !name.empty() && name.back() != '_')
            name += '_';
        if (name.size() == 20)
            break;
    }

    while (name.size() < 3)
        name += '_';
    return name;
}
}
