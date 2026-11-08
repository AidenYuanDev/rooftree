/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Crypto at the sizes the framework uses it: token and handshake hashes, TOTP, random bytes, and
 * password hashing (Argon2, slow on purpose).
 */

#include "rooftree_benchmark.h"

#include "Argon2Hash.h"
#include "CryptoHash.h"
#include "CryptoRandom.h"
#include "TOTP.h"

namespace
{
std::vector<uint8> RandomData(std::size_t size)
{
    std::vector<uint8> data(size);
    Rooftree::Crypto::GetRandomBytes(data);
    return data;
}
}

TEST_CASE("Hashes", "[crypto]")
{
    for (std::size_t size : { 64, 4096 })
    {
        std::vector<uint8> data = RandomData(size);
        int bytes = int(size);
        BENCHMARK(Bench::Name("SHA1", bytes, "bytes")) { return Rooftree::Crypto::SHA1::GetDigestOf(data); };
        BENCHMARK(Bench::Name("SHA256", bytes, "bytes")) { return Rooftree::Crypto::SHA256::GetDigestOf(data); };
    }
}

TEST_CASE("Tokens", "[crypto]")
{
    std::vector<uint8> secret = RandomData(Rooftree::Crypto::TOTP::SecretSize);
    BENCHMARK("TOTP GenerateToken") { return Rooftree::Crypto::TOTP::GenerateToken(secret, 59); };
    BENCHMARK("GetRandomBytes<32>") { return Rooftree::Crypto::GetRandomBytes<32>(); };
}

TEST_CASE("Argon2", "[crypto][argon2]")
{
    // Cheap parameters, like the unit test
    BENCHMARK("Hash (2 iterations, 1 MiB)") { return Rooftree::Crypto::Argon2::Hash("password", 2, 1024); };

    Optional<std::string> hash = Rooftree::Crypto::Argon2::Hash("password", 2, 1024);
    REQUIRE(hash);
    BENCHMARK("Verify (2 iterations, 1 MiB)") { return Rooftree::Crypto::Argon2::Verify("password", *hash); };

    // What a login costs with the blog's and the arena's defaults (OWASP's minimum for Argon2id)
    BENCHMARK("Hash (2 iterations, 19 MiB)") { return Rooftree::Crypto::Argon2::Hash("password", 2, 19456); };
}
