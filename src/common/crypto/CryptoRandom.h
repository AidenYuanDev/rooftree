/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Random bytes for secrets (tokens, keys, salts, TOTP secrets), from OpenSSL's generator. Running out
 * of them is a broken system, not an error to handle: it asserts.
 */

#ifndef ROOFTREE_CRYPTO_RANDOM_H
#define ROOFTREE_CRYPTO_RANDOM_H

#include "Define.h"
#include <array>
#include <span>

namespace Rooftree::Crypto
{
void GetRandomBytes(uint8* bytes, std::size_t size);

inline void GetRandomBytes(std::span<uint8> bytes) { GetRandomBytes(bytes.data(), bytes.size()); }

template<std::size_t Size>
std::array<uint8, Size> GetRandomBytes()
{
    std::array<uint8, Size> bytes;
    GetRandomBytes(bytes.data(), bytes.size());
    return bytes;
}
}

#endif // ROOFTREE_CRYPTO_RANDOM_H
