/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "CryptoRandom.h"
#include "Errors.h"
#include <openssl/rand.h>
#include <algorithm>
#include <limits>

namespace Rooftree::Crypto
{
void GetRandomBytes(uint8* bytes, std::size_t size)
{
    while (size)
    {
        int chunk = int(std::min<std::size_t>(size, std::numeric_limits<int>::max()));
        ASSERT(RAND_bytes(bytes, chunk) == 1, "OpenSSL has no random bytes");
        bytes += chunk;
        size -= std::size_t(chunk);
    }
}
}
