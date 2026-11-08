/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * RFC 6238 time-based one-time passwords as authenticator apps make them: HMAC-SHA1, 30 second steps,
 * 6 digits.
 */

#ifndef ROOFTREE_TOTP_H
#define ROOFTREE_TOTP_H

#include "Define.h"
#include <ctime>
#include <span>

namespace Rooftree::Crypto::TOTP
{
// As long as the HMAC-SHA1 key RFC 4226 recommends
constexpr std::size_t SecretSize = 20;

// The code for the step time falls in
uint32 GenerateToken(std::span<uint8 const> secret, time_t time);
}

#endif // ROOFTREE_TOTP_H
