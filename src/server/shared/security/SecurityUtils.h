/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * The small pieces web logins are built from:
 *
 *  - tokens (session ids, CSRF tokens, mail links): 32 random bytes from OpenSSL, base64url. The
 *    database only ever stores Sha256Hex(token), so a leaked table can't be replayed as cookies
 *  - ConstantTimeEquals for comparing secrets, so the time a mismatch takes doesn't reveal how many
 *    leading characters were right
 *  - CSRF: a state-changing request (not GET/HEAD/OPTIONS) must come from one of the site's own
 *    origins (Origin header, or Referer when a browser leaves Origin out) and carry the session's
 *    token in X-CSRF-Token. A cross-site form can't set that header and can't read the token
 *  - base32 and otpauth:// URIs for TOTP secrets (Rooftree::Crypto::TOTP), which authenticator apps
 *    scan as QR codes
 */

#ifndef ROOFTREE_SECURITY_UTILS_H
#define ROOFTREE_SECURITY_UTILS_H

#include "Define.h"
#include "HttpCommon.h"
#include "Optional.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rooftree::Server
{
// RFC 4648 base64 with padding, e.g. for data: URLs
std::string Base64Encode(std::span<uint8 const> data);
std::string Base64UrlEncode(std::span<uint8 const> data);
Optional<std::vector<uint8>> Base64UrlDecode(std::string_view text);

// RFC 4648 base32 without padding, what otpauth:// URIs use
std::string Base32Encode(std::span<uint8 const> data);
Optional<std::vector<uint8>> Base32Decode(std::string_view text);

// size random bytes, base64url
std::string GenerateToken(std::size_t size = 32);

std::string Sha256Hex(std::string_view data);

bool ConstantTimeEquals(std::string_view left, std::string_view right);

bool IsSafeMethod(boost::beast::http::verb method);

// "https://blog.example.com" of the Origin header, or of the Referer when there is no Origin. Empty
// when neither is there
std::string GetRequestOrigin(Net::Http::Request const& request);

// The request's origin is one of allowedOrigins (exact, scheme://host[:port])
bool CheckOrigin(Net::Http::Request const& request, std::span<std::string const> allowedOrigins);

// X-CSRF-Token equals the expected token
bool CheckCsrfToken(Net::Http::Request const& request, std::string_view expectedToken);

// otpauth://totp/<issuer>:<account>?secret=<base32>&issuer=<issuer>
std::string BuildTotpUri(std::string_view issuer, std::string_view account, std::span<uint8 const> secret);
}

#endif // ROOFTREE_SECURITY_UTILS_H
