/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Text from a {fmt} format, checked when compiled: StringFormat("{} of {}", a, b). Enums print as
 * their number, an empty Optional as "(none)". FormatString and FormatArgs are for functions that
 * pass a format on (the log, CommandHandler::PSendSysMessage).
 */

#ifndef ROOFTREE_STRING_FORMAT_H
#define ROOFTREE_STRING_FORMAT_H

#include "Optional.h"
// format.h, not core.h: with fmt 12 the std::string formatter is there, and GCC rejects its
// specialization once core.h alone instantiated it
#include <fmt/format.h>
#include <string>
#include <type_traits>

namespace Rooftree
{
template<typename... Args>
using FormatString = fmt::format_string<Args...>;
using FormatStringView = fmt::string_view;
using FormatArgs = fmt::format_args;

template<typename... Args>
std::string StringFormat(FormatString<Args...> format, Args&&... args)
{
    return fmt::format(format, std::forward<Args>(args)...);
}

// Appended at out, an output iterator
template<typename OutputIt, typename... Args>
OutputIt StringFormatTo(OutputIt out, FormatString<Args...> format, Args&&... args)
{
    return fmt::format_to(out, format, std::forward<Args>(args)...);
}

template<typename... Args>
auto MakeFormatArgs(Args&... args) { return fmt::make_format_args(args...); }

// A format checked by the caller's FormatString; what doesn't fit says so in the text instead of throwing
std::string StringVFormat(FormatStringView format, FormatArgs args) noexcept;
}

template<typename T>
struct fmt::formatter<Optional<T>> : fmt::formatter<T>
{
    auto format(Optional<T> const& value, fmt::format_context& context) const
    {
        if (!value)
            return fmt::format_to(context.out(), "(none)");
        return fmt::formatter<T>::format(*value, context);
    }
};

template<typename E> requires std::is_enum_v<E>
constexpr auto format_as(E value) { return std::underlying_type_t<E>(value); }

#endif // ROOFTREE_STRING_FORMAT_H
