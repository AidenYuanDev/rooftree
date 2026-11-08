/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Logging in with GitHub or Google (OAuth 2.0 authorization code flow with PKCE), the provider side:
 * the address to send the browser to, and on a worker, the code exchanged for a token and the
 * provider's user read with it (id, login, name, a verified address). The flow itself, accounts and
 * sessions are in OAuthHandlers.cpp.
 *
 * A provider is on when it has a client id and secret (Blog.OAuth.<Provider>); its endpoints have
 * defaults and are only set for tests, against a fake provider.
 */

#ifndef BLOG_OAUTH_H
#define BLOG_OAUTH_H

#include "Optional.h"
#include <string>
#include <string_view>
#include <vector>

class Config;

namespace Blog
{
struct OAuthProvider
{
    std::string Name;           // "github", "google": in URLs and the user_identity table
    std::string ClientId;
    std::string ClientSecret;
    std::string AuthorizeUrl;
    std::string TokenUrl;
    std::string UserUrl;
    std::string EmailsUrl;      // GitHub's list of addresses, the user's own may be private

    bool IsEnabled() const { return !ClientId.empty() && !ClientSecret.empty(); }

    // Blog.OAuth.GitHub, Blog.OAuth.Google
    static std::vector<OAuthProvider> FromConfig(Config const& config);
};

struct OAuthIdentity
{
    std::string ProviderUserId;
    std::string Login;          // GitHub's login, the Google address: what the account page shows
    std::string Name;
    std::string Email;          // empty unless the provider has verified it
    std::string AvatarUrl;      // the provider's picture, empty when it has none (or no https:// one)
};

// The PKCE pair: a random verifier kept by the server, its S256 challenge sent with the browser
struct OAuthPkce
{
    std::string Verifier;
    std::string Challenge;

    static OAuthPkce Generate();
};

std::string BuildOAuthAuthorizeUrl(OAuthProvider const& provider, std::string_view redirectUri, std::string_view state, std::string_view codeChallenge);

// Worker: the code of the callback exchanged for a token, the user read with it. Empty with error set
// when the provider refused or didn't answer
Optional<OAuthIdentity> WorkerFetchOAuthIdentity(OAuthProvider const& provider, std::string const& redirectUri, std::string const& code,
    std::string const& codeVerifier, std::string& error);

// A user name for a new account from the provider's login or name: letters, digits and _, 3 to 20
std::string SuggestUsername(OAuthIdentity const& identity);
}

#endif // BLOG_OAUTH_H
