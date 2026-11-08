/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Quick random numbers for the game and pictures (where a coin lands, a captcha's noise): one
 * generator per thread, seeded from the system. Not for secrets: tokens, keys and salts come from
 * Crypto::GetRandomBytes (CryptoRandom.h).
 */

#ifndef ROOFTREE_RANDOM_H
#define ROOFTREE_RANDOM_H

#include "Define.h"
#include "Duration.h"

// Both ends included
uint32 urand(uint32 min, uint32 max);
// [min, max)
float frand(float min, float max);
Milliseconds randtime(Milliseconds min, Milliseconds max);
uint32 rand32();

#endif // ROOFTREE_RANDOM_H
