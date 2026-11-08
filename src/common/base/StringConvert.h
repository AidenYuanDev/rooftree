/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * StringTo<T>(text): the whole text as a T, or nothing. No spaces around it and no '+', so what comes
 * from a URL or a config value is either exactly a number or rejected.
 *
 *   integers  in base (10 by default); base 0 reads a 0x or 0b prefix, decimal without one
 *   floating  decimal or scientific (1.5, 2e-3), "inf" and "nan" too
 *   bool      1 true yes on y, 0 false no off n, in any case
 */

#ifndef ROOFTREE_STRING_CONVERT_H
#define ROOFTREE_STRING_CONVERT_H

#include "Optional.h"
#include "Util.h"
#include <charconv>
#include <string_view>
#include <type_traits>

namespace Rooftree
{
namespace Impl
{
template<typename T>
Optional<T> FromChars(std::string_view text, auto... format)
{
    T value;
    char const* end = text.data() + text.size();
    auto [stop, error] = std::from_chars(text.data(), end, value, format...);
    if (text.empty() || error != std::errc() || stop != end)
        return {};
    return value;
}
}

template<typename T>
Optional<T> StringTo(std::string_view text, int base = 10)
{
    if constexpr (std::is_same_v<T, bool>)
    {
        for (std::string_view yes : { "1", "true", "yes", "on", "y" })
            if (StringEqualI(text, yes))
                return true;
        for (std::string_view no : { "0", "false", "no", "off", "n" })
            if (StringEqualI(text, no))
                return false;
        return {};
    }
    else if constexpr (std::is_integral_v<T>)
    {
        if (base == 0)
        {
            base = 10;
            if (StringEqualI(text.substr(0, 2), "0x"))
                base = 16;
            else if (StringEqualI(text.substr(0, 2), "0b"))
                base = 2;
            if (base != 10)
                text.remove_prefix(2);
        }
        return Impl::FromChars<T>(text, base);
    }
    else
    {
        static_assert(std::is_floating_point_v<T>, "StringTo: an integer, a floating point number or bool");
        return Impl::FromChars<T>(text, std::chars_format::general);
    }
}
}

#endif // ROOFTREE_STRING_CONVERT_H
