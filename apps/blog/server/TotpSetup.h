/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * What sets an authenticator app up for a new TOTP secret, printed by --create-admin and the console's
 * "account set 2fa": the otpauth link as a QR code, the link, and the secret to enter by hand.
 */

#ifndef BLOG_TOTP_SETUP_H
#define BLOG_TOTP_SETUP_H

#include "Define.h"
#include <span>
#include <string>
#include <string_view>

namespace Blog
{
// The QR code only for a terminal (it has escape sequences). The API tests read the secret after "by hand: "
std::string FormatTotpSetup(std::string_view issuer, std::string_view account, std::span<uint8 const> secret, bool terminal);
}

#endif // BLOG_TOTP_SETUP_H
