/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Passwords as Argon2id hashes, encoded with their parameters and a salt of their own
 * ($argon2id$v=19$m=19456,t=2,p=1$salt$hash), so Verify needs only the stored text: hashes made with
 * other parameters before still verify. Slow on purpose: on a worker, never the main thread.
 */

#ifndef ROOFTREE_ARGON2_HASH_H
#define ROOFTREE_ARGON2_HASH_H

#include "Define.h"
#include "Optional.h"
#include <string>

namespace Rooftree::Crypto::Argon2
{
// iterations: passes over the memory; memoryCost: KiB. Nothing if libargon2 refuses them
Optional<std::string> Hash(std::string const& password, uint32 iterations, uint32 memoryCost);
bool Verify(std::string const& password, std::string const& hash);
}

#endif // ROOFTREE_ARGON2_HASH_H
