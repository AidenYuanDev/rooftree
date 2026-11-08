/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * Small text helpers: splitting, hex, UTF-8 lengths, durations as people type them.
 */

#ifndef ROOFTREE_UTIL_H
#define ROOFTREE_UTIL_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Rooftree
{
// The parts of text between the separators, views into text (which has to outlive them). keepEmpty: also
// the empty ones, "a,,b" gives three
std::vector<std::string_view> Tokenize(std::string_view text, char separator, bool keepEmpty);

// ASCII letters in any case
bool StringEqualI(std::string_view a, std::string_view b);
bool StringStartsWithI(std::string_view text, std::string_view prefix);

// ASCII only, other bytes stay (the C functions depend on the locale and take an int)
constexpr char CharToLower(char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
constexpr char CharToUpper(char c) { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }

// Upper case hex, two digits a byte (the hashes stored in the database are)
std::string ToHex(std::span<uint8 const> bytes);
// Nothing unless every character is a hex digit and there is an even number of them
Optional<std::vector<uint8>> FromHex(std::string_view hex);

// Code points of UTF-8 text, 0 if it isn't valid UTF-8
std::size_t Utf8Length(std::string_view text);
// To its first length code points; invalid UTF-8 is cleared
void Utf8Truncate(std::string& text, std::size_t length);

// "1d12h", "30m", "90s", "1h30m": the units d h m s, each once at most, in that order. Nothing for
// anything else, a number without a unit included
Optional<Seconds> ParseDuration(std::string_view text);
// The other way: 1d2h3m4s, units that are 0 left out ("0s" for none)
std::string FormatDuration(Seconds duration);
}

#endif // ROOFTREE_UTIL_H
