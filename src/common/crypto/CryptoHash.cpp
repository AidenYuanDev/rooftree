/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "CryptoHash.h"
#include "Errors.h"
#include <openssl/evp.h>
#include <memory>

namespace Rooftree::Crypto::Impl
{
void Digest(HashAlgorithm algorithm, std::initializer_list<std::span<uint8 const>> parts, uint8* out)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    EVP_MD const* md = algorithm == HashAlgorithm::SHA1 ? EVP_sha1() : EVP_sha256();
    bool ok = context && EVP_DigestInit_ex(context.get(), md, nullptr);
    for (std::span<uint8 const> part : parts)
        ok = ok && EVP_DigestUpdate(context.get(), part.data(), part.size());
    ok = ok && EVP_DigestFinal_ex(context.get(), out, nullptr);
    // Out of memory or a broken OpenSSL
    ASSERT(ok, "OpenSSL failed to hash");
}
}
