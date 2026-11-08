/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "SecurityUtils.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "StringFormat.h"
#include "Util.h"
#include <boost/url/encode.hpp>
#include <boost/url/rfc/unreserved_chars.hpp>
#include <boost/url/parse.hpp>
#include <openssl/crypto.h>

namespace Rooftree::Server
{
namespace
{
constexpr std::string_view Base64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr std::string_view Base64UrlAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view Base32Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

// Bit packing shared by both encodings: bitsPerChar of the input per output character
std::string Encode(std::span<uint8 const> data, std::string_view alphabet, uint32 bitsPerChar)
{
    std::string text;
    text.reserve((data.size() * 8 + bitsPerChar - 1) / bitsPerChar);
    uint32 buffer = 0;
    uint32 bits = 0;
    for (uint8 byte : data)
    {
        buffer = (buffer << 8) | byte;
        bits += 8;
        while (bits >= bitsPerChar)
        {
            bits -= bitsPerChar;
            text += alphabet[(buffer >> bits) & ((1u << bitsPerChar) - 1)];
        }
    }

    if (bits)
        text += alphabet[(buffer << (bitsPerChar - bits)) & ((1u << bitsPerChar) - 1)];

    return text;
}

Optional<std::vector<uint8>> Decode(std::string_view text, std::string_view alphabet, uint32 bitsPerChar, bool ignoreCase)
{
    std::vector<uint8> data;
    data.reserve(text.size() * bitsPerChar / 8);
    uint32 buffer = 0;
    uint32 bits = 0;
    for (char c : text)
    {
        std::size_t value = alphabet.find(ignoreCase ? Rooftree::CharToUpper(c) : c);
        if (value == std::string_view::npos)
            return {};

        buffer = (buffer << bitsPerChar) | uint32(value);
        bits += bitsPerChar;
        if (bits >= 8)
        {
            bits -= 8;
            data.push_back(uint8(buffer >> bits));
        }
    }

    // Leftover bits are padding and must be zero, otherwise two texts would decode to the same bytes
    if (bits >= bitsPerChar || (buffer & ((1u << bits) - 1)))
        return {};

    return data;
}

// scheme://host[:port] of an absolute URL, lower case scheme and host
std::string GetOrigin(std::string_view url)
{
    boost::system::result<boost::urls::url_view> parsed = boost::urls::parse_absolute_uri(url);
    if (!parsed || !parsed->has_authority() || parsed->encoded_host().empty())
        return {};

    std::string origin(parsed->scheme());
    origin += "://";
    origin += parsed->encoded_host();
    if (parsed->has_port())
    {
        origin += ':';
        origin += parsed->port();
    }

    std::ranges::transform(origin, origin.begin(), Rooftree::CharToLower);
    return origin;
}
}

std::string Base64Encode(std::span<uint8 const> data)
{
    std::string text = Encode(data, Base64Alphabet, 6);
    text.append((4 - text.size() % 4) % 4, '=');
    return text;
}

std::string Base64UrlEncode(std::span<uint8 const> data)
{
    return Encode(data, Base64UrlAlphabet, 6);
}

Optional<std::vector<uint8>> Base64UrlDecode(std::string_view text)
{
    return Decode(text, Base64UrlAlphabet, 6, false);
}

std::string Base32Encode(std::span<uint8 const> data)
{
    return Encode(data, Base32Alphabet, 5);
}

Optional<std::vector<uint8>> Base32Decode(std::string_view text)
{
    return Decode(text, Base32Alphabet, 5, true);
}

std::string GenerateToken(std::size_t size)
{
    std::vector<uint8> bytes(size);
    Crypto::GetRandomBytes(bytes);
    return Base64UrlEncode(bytes);
}

std::string Sha256Hex(std::string_view data)
{
    Crypto::SHA256::Digest digest = Crypto::SHA256::GetDigestOf(reinterpret_cast<uint8 const*>(data.data()), data.size());
    return Rooftree::ToHex(digest);
}

bool ConstantTimeEquals(std::string_view left, std::string_view right)
{
    // The length is not secret: tokens have a fixed one
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

bool IsSafeMethod(boost::beast::http::verb method)
{
    return method == boost::beast::http::verb::get || method == boost::beast::http::verb::head || method == boost::beast::http::verb::options;
}

std::string GetRequestOrigin(Net::Http::Request const& request)
{
    std::string_view origin = Net::Http::ToStdStringView(request[boost::beast::http::field::origin]);
    // "null" is what a sandboxed or file:// page sends, never one of ours
    if (!origin.empty())
        return origin == "null" ? std::string() : GetOrigin(origin);

    return GetOrigin(Net::Http::ToStdStringView(request[boost::beast::http::field::referer]));
}

bool CheckOrigin(Net::Http::Request const& request, std::span<std::string const> allowedOrigins)
{
    std::string origin = GetRequestOrigin(request);
    return !origin.empty() && std::ranges::find(allowedOrigins, origin) != allowedOrigins.end();
}

bool CheckCsrfToken(Net::Http::Request const& request, std::string_view expectedToken)
{
    std::string_view token = Net::Http::ToStdStringView(request["X-CSRF-Token"]);
    return !expectedToken.empty() && ConstantTimeEquals(token, expectedToken);
}

std::string BuildTotpUri(std::string_view issuer, std::string_view account, std::span<uint8 const> secret)
{
    std::string label = boost::urls::encode(issuer, boost::urls::unreserved_chars);
    label += ':';
    label += boost::urls::encode(account, boost::urls::unreserved_chars);

    return StringFormat("otpauth://totp/{}?secret={}&issuer={}&algorithm=SHA1&digits=6&period=30",
        label, Base32Encode(secret), boost::urls::encode(issuer, boost::urls::unreserved_chars));
}
}
