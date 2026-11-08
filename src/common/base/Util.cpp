/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Util.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <utf8.h>
#include <algorithm>

namespace Rooftree
{
std::vector<std::string_view> Tokenize(std::string_view text, char separator, bool keepEmpty)
{
    std::vector<std::string_view> parts;
    for (;;)
    {
        std::size_t end = text.find(separator);
        std::string_view part = text.substr(0, end);
        if (keepEmpty || !part.empty())
            parts.push_back(part);
        if (end == std::string_view::npos)
            return parts;
        text.remove_prefix(end + 1);
    }
}

bool StringEqualI(std::string_view a, std::string_view b)
{
    return std::ranges::equal(a, b, [](char x, char y) { return CharToLower(x) == CharToLower(y); });
}

bool StringStartsWithI(std::string_view text, std::string_view prefix)
{
    return text.size() >= prefix.size() && StringEqualI(text.substr(0, prefix.size()), prefix);
}

std::string ToHex(std::span<uint8 const> bytes)
{
    static constexpr char Digits[] = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (uint8 byte : bytes)
    {
        hex += Digits[byte >> 4];
        hex += Digits[byte & 0xF];
    }
    return hex;
}

Optional<std::vector<uint8>> FromHex(std::string_view hex)
{
    if (hex.size() % 2)
        return {};

    std::vector<uint8> bytes;
    bytes.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        Optional<uint8> byte = StringTo<uint8>(hex.substr(i, 2), 16);
        // from_chars takes a sign
        if (!byte || hex[i] == '-')
            return {};
        bytes.push_back(*byte);
    }
    return bytes;
}

std::size_t Utf8Length(std::string_view text)
{
    if (!utf8::is_valid(text.begin(), text.end()))
        return 0;
    return std::size_t(utf8::distance(text.begin(), text.end()));
}

void Utf8Truncate(std::string& text, std::size_t length)
{
    if (!utf8::is_valid(text.begin(), text.end()))
    {
        text.clear();
        return;
    }

    auto end = text.begin();
    for (std::size_t i = 0; i < length && end != text.end(); ++i)
        utf8::unchecked::next(end);
    text.erase(end, text.end());
}

Optional<Seconds> ParseDuration(std::string_view text)
{
    static constexpr std::pair<char, Seconds> Units[] = { { 'd', 24h }, { 'h', 1h }, { 'm', 1min }, { 's', 1s } };

    Seconds total{};
    std::size_t nextUnit = 0;
    while (!text.empty())
    {
        std::size_t digits = text.find_first_not_of("0123456789");
        if (digits == 0 || digits == std::string_view::npos)
            return {};

        auto unit = std::find_if(std::begin(Units) + nextUnit, std::end(Units), [&](auto const& u) { return u.first == text[digits]; });
        Optional<uint32> count = StringTo<uint32>(text.substr(0, digits));
        if (unit == std::end(Units) || !count)
            return {};

        total += *count * unit->second;
        nextUnit = std::size_t(unit - std::begin(Units)) + 1;
        text.remove_prefix(digits + 1);
    }
    if (!nextUnit)
        return {};
    return total;
}

std::string FormatDuration(Seconds duration)
{
    int64 seconds = duration.count();
    std::string text;
    for (auto [unit, length] : { std::pair{ 'd', 86400 }, std::pair{ 'h', 3600 }, std::pair{ 'm', 60 } })
    {
        if (seconds >= length)
            text += StringFormat("{}{}", seconds / length, unit);
        seconds %= length;
    }
    if (seconds || text.empty())
        text += StringFormat("{}s", seconds);
    return text;
}
}
