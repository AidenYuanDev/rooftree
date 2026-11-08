/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Argon2Hash.h"
#include "CryptoRandom.h"
#include <argon2.h>
#include <array>

namespace Rooftree::Crypto::Argon2
{
namespace
{
constexpr uint32 SaltSize = 16;
constexpr uint32 HashSize = 16;
constexpr uint32 Lanes = 1;
}

Optional<std::string> Hash(std::string const& password, uint32 iterations, uint32 memoryCost)
{
    std::array<uint8, SaltSize> salt = GetRandomBytes<SaltSize>();
    std::string encoded(argon2_encodedlen(iterations, memoryCost, Lanes, SaltSize, HashSize, Argon2_id), '\0');
    if (argon2id_hash_encoded(iterations, memoryCost, Lanes, password.data(), password.size(), salt.data(), salt.size(), HashSize,
        encoded.data(), encoded.size()) != ARGON2_OK)
        return {};

    // The length counts the terminating zero
    if (std::size_t end = encoded.find('\0'); end != std::string::npos)
        encoded.resize(end);
    return encoded;
}

bool Verify(std::string const& password, std::string const& hash)
{
    return argon2id_verify(hash.c_str(), password.data(), password.size()) == ARGON2_OK;
}
}
