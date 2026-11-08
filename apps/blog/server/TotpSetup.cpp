/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "TotpSetup.h"
#include "Optional.h"
#include "SecurityUtils.h"
#include "StringFormat.h"
#include <memory>
#include <qrencode.h>

namespace Blog
{
namespace
{
// Two rows of modules per line with half blocks, light modules drawn in white on black so it scans
// whatever the terminal's colors, a 4 module quiet zone around it
Optional<std::string> RenderQrCode(std::string const& text)
{
    std::unique_ptr<QRcode, decltype(&QRcode_free)> code(QRcode_encodeString(text.c_str(), 0, QR_ECLEVEL_M, QR_MODE_8, 1), &QRcode_free);
    if (!code)
        return {};

    int const margin = 4;
    int const size = code->width + 2 * margin;
    auto dark = [&](int x, int y)
    {
        x -= margin;
        y -= margin;
        return x >= 0 && y >= 0 && x < code->width && y < code->width && (code->data[y * code->width + x] & 1);
    };

    std::string out;
    for (int y = 0; y < size; y += 2)
    {
        out += "  \x1b[37;40m";
        for (int x = 0; x < size; ++x)
        {
            bool top = !dark(x, y);
            bool bottom = y + 1 < size && !dark(x, y + 1);
            out += top && bottom ? "█" : top ? "▀" : bottom ? "▄" : " ";
        }
        out += "\x1b[0m\n";
    }
    return out;
}
}

std::string FormatTotpSetup(std::string_view issuer, std::string_view account, std::span<uint8 const> secret, bool terminal)
{
    std::string uri = Rooftree::Server::BuildTotpUri(issuer, account, secret);
    std::string text;
    Optional<std::string> qrCode = terminal ? RenderQrCode(uri) : Optional<std::string>();
    if (qrCode)
        text = *qrCode + "\nScan this with an authenticator app, or add the link:\n\n";
    else
        text = "Add this to an authenticator app (most can scan it as a QR code, e.g. qrencode -t ansiutf8 '<uri>'):\n\n";

    text += Rooftree::StringFormat("  {}\n\nor enter the secret by hand: {}\n\nLogging in asks for the 6-digit code.", uri,
        Rooftree::Server::Base32Encode(secret));
    return text;
}
}
