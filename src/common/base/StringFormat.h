/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Text from a std::format format, checked when compiled: StringFormat("{} of {}", a, b).
 * FormatString and FormatArgs are for functions that pass
 * a format on (the log, CommandHandler::PSendSysMessage).
 *
 * What std::format doesn't print by itself:
 *   an Optional    OrNone(value): the value, or "(none)" when there is none
 *   an enum        std::to_underlying(value): its number
 */

#ifndef ROOFTREE_STRING_FORMAT_H
#define ROOFTREE_STRING_FORMAT_H

#include "Optional.h"

#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace Rooftree {
template <typename... Args>
using FormatString = std::format_string<Args...>;
using FormatStringView = std::string_view;
using FormatArgs = std::format_args;

template <typename... Args>
std::string StringFormat(FormatString<Args...> format, Args&&... args) {
  return std::format(format, std::forward<Args>(args)...);
}

// Appended at out, an output iterator
template <typename OutputIt, typename... Args>
OutputIt StringFormatTo(OutputIt out, FormatString<Args...> format, Args&&... args) {
  return std::format_to(out, format, std::forward<Args>(args)...);
}

template <typename... Args>
auto MakeFormatArgs(Args&... args) {
  return std::make_format_args(args...);
}

// A format checked by the caller's FormatString; what
// doesn't fit says so in the text instead of throwing
std::string StringVFormat(FormatStringView format, FormatArgs args) noexcept;

// An Optional to format: std::formatter can't be specialized for std::optional of a standard type
template <typename T>
struct NoneOr {
  Optional<T> const& Value;
};

template <typename T>
NoneOr<T> OrNone(Optional<T> const& value) {
  return {value};
}
}  // namespace Rooftree

// The value's own format spec: "{:>5}" pads the value, "(none)" stays as it is
template <typename T>
struct std::formatter<Rooftree::NoneOr<T>> : std::formatter<T> {
  auto format(Rooftree::NoneOr<T> const& value, std::format_context& context) const {
    if (!value.Value)
      return std::format_to(context.out(), "(none)");
    return std::formatter<T>::format(*value.Value, context);
  }
};

#endif  // ROOFTREE_STRING_FORMAT_H
