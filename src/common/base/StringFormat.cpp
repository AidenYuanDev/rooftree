/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "StringFormat.h"

namespace Rooftree
{
std::string StringVFormat(FormatStringView format, FormatArgs args) noexcept
{
    try
    {
        return fmt::vformat(format, args);
    }
    catch (std::exception const& error)
    {
        return fmt::format("(cannot format \"{}\": {})", format, error.what());
    }
}
}
