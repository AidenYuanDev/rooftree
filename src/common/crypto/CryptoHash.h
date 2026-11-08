/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * SHA-1 (the WebSocket handshake) and SHA-256 (token hashes, PKCE) through OpenSSL:
 *
 *   SHA256::Digest digest = SHA256::GetDigestOf(text);              // one part
 *   SHA1::Digest accept = SHA1::GetDigestOf(key, Guid);              // the digest of the parts one after the other
 *
 * A part is text (anything that converts to std::string_view) or bytes (a span of uint8).
 */

#ifndef ROOFTREE_CRYPTO_HASH_H
#define ROOFTREE_CRYPTO_HASH_H

#include "Define.h"
#include <array>
#include <initializer_list>
#include <span>
#include <string_view>
#include <type_traits>

namespace Rooftree::Crypto
{
enum class HashAlgorithm { SHA1, SHA256 };

namespace Impl
{
void Digest(HashAlgorithm algorithm, std::initializer_list<std::span<uint8 const>> parts, uint8* out);

template<typename Part>
concept HashPart = std::is_convertible_v<Part const&, std::string_view> || std::is_convertible_v<Part const&, std::span<uint8 const>>;

template<typename Part>
std::span<uint8 const> BytesOf(Part const& part)
{
    if constexpr (std::is_convertible_v<Part const&, std::string_view>)
    {
        std::string_view text = part;
        return { reinterpret_cast<uint8 const*>(text.data()), text.size() };
    }
    else
        return std::span<uint8 const>(part);
}
}

template<HashAlgorithm Algorithm, std::size_t Size>
struct Hash
{
    using Digest = std::array<uint8, Size>;

    template<Impl::HashPart... Parts>
    static Digest GetDigestOf(Parts const&... parts)
    {
        Digest digest;
        Impl::Digest(Algorithm, { Impl::BytesOf(parts)... }, digest.data());
        return digest;
    }

    static Digest GetDigestOf(uint8 const* data, std::size_t size) { return GetDigestOf(std::span<uint8 const>(data, size)); }
};

using SHA1 = Hash<HashAlgorithm::SHA1, 20>;
using SHA256 = Hash<HashAlgorithm::SHA256, 32>;
}

#endif // ROOFTREE_CRYPTO_HASH_H
