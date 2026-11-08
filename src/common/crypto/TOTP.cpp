/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "TOTP.h"
#include "Errors.h"
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace Rooftree::Crypto::TOTP
{
uint32 GenerateToken(std::span<uint8 const> secret, time_t time)
{
    // The step as 8 bytes, big endian
    uint64 step = uint64(time) / 30;
    uint8 message[8];
    for (int i = 7; i >= 0; --i, step >>= 8)
        message[i] = uint8(step);

    uint8 digest[20];
    unsigned int digestSize = sizeof(digest);
    ASSERT(HMAC(EVP_sha1(), secret.data(), int(secret.size()), message, sizeof(message), digest, &digestSize), "OpenSSL failed HMAC-SHA1");

    // RFC 4226's dynamic truncation: 31 bits from where the last nibble says
    std::size_t offset = digest[19] & 0x0F;
    uint32 bits = (uint32(digest[offset] & 0x7F) << 24) | (uint32(digest[offset + 1]) << 16) | (uint32(digest[offset + 2]) << 8)
        | uint32(digest[offset + 3]);
    return bits % 1000000;
}
}
