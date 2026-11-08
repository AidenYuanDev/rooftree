/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "Validation.h"
#include "Define.h"
#include <utf8.h>
#include <algorithm>
#include <cctype>
#include <regex>

namespace Blog
{
namespace
{
std::string_view Trim(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\r\n";
    std::size_t begin = text.find_first_not_of(whitespace);
    if (begin == std::string_view::npos)
        return {};

    return text.substr(begin, text.find_last_not_of(whitespace) - begin + 1);
}

bool IsAsciiLowerOrDigit(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
}

std::size_t CountCharacters(std::string_view text)
{
    return std::size_t(utf8::distance(text.begin(), text.end()));
}

std::u32string MatchableText(std::string_view text)
{
    // [text](target "title") shows its text only
    static std::regex const linkTarget(R"(\]\([^)]*\))");
    std::string shown = std::regex_replace(std::string(text), linkTarget, "]");

    std::u32string matchable;
    matchable.reserve(shown.size());
    auto itr = shown.begin();
    while (itr != shown.end())
    {
        uint32 c = 0;
        try
        {
            c = utf8::next(itr, shown.end());
        }
        catch (utf8::exception const&)
        {
            ++itr;
            continue;
        }

        if (c < 0x80)
        {
            if (std::isalnum(int(c)))
                matchable.push_back(char32_t(std::tolower(int(c))));
            continue;
        }

        bool punctuation = (c >= 0x00A0 && c <= 0x00BF)    // Latin-1 spaces and signs
            || (c >= 0x2000 && c <= 0x206F)                 // general punctuation: curly quotes, dashes, ellipsis
            || (c >= 0x3000 && c <= 0x303F)                 // CJK symbols and punctuation
            || (c >= 0xFF00 && c <= 0xFF0F) || (c >= 0xFF1A && c <= 0xFF20)
            || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65);   // fullwidth punctuation
        if (!punctuation)
            matchable.push_back(char32_t(c));
    }
    return matchable;
}

bool IsCleanText(std::string_view text, bool multiLine)
{
    return std::ranges::none_of(text, [multiLine](char c)
    {
        unsigned char u = static_cast<unsigned char>(c);
        if (multiLine && (c == '\n' || c == '\t' || c == '\r'))
            return false;
        return u < 0x20 || u == 0x7F;
    });
}

bool IsCjk(uint32 c)
{
    return (c >= 0x3040 && c <= 0x30FF)         // kana
        || (c >= 0x3400 && c <= 0x4DBF)         // CJK extension A
        || (c >= 0x4E00 && c <= 0x9FFF)         // CJK unified ideographs
        || (c >= 0xAC00 && c <= 0xD7AF)         // Hangul syllables
        || (c >= 0xF900 && c <= 0xFAFF)         // compatibility ideographs
        || (c >= 0x20000 && c <= 0x3134F);      // extensions B to G
}

bool IsWordCharacter(uint32 c)
{
    if (c < 0x80)
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    return !(c <= 0xBF || (c >= 0x2000 && c <= 0x2BFF) || (c >= 0x3000 && c <= 0x303F) || (c >= 0xFE30 && c <= 0xFE4F)
        || (c >= 0xFF00 && c <= 0xFF20) || (c >= 0xFF3B && c <= 0xFF40) || (c >= 0xFF5B && c <= 0xFF65) || c >= 0x1F000);
}

namespace
{
// markdown without fenced and inline code, image targets and link targets
std::string StripMarkdownCode(std::string_view markdown)
{
    std::string text;
    text.reserve(markdown.size());
    std::size_t i = 0;
    while (i < markdown.size())
    {
        if (markdown.substr(i, 3) == "```")
        {
            std::size_t end = markdown.find("```", i + 3);
            i = end == std::string_view::npos ? markdown.size() : end + 3;
            text += ' ';
        }
        else if (markdown[i] == '`')
        {
            std::size_t end = markdown.find('`', i + 1);
            i = end == std::string_view::npos ? markdown.size() : end + 1;
            text += ' ';
        }
        else if (markdown.substr(i, 2) == "](")
        {
            std::size_t end = markdown.find(')', i + 2);
            i = end == std::string_view::npos ? markdown.size() : end + 1;
            text += ' ';
        }
        else
            text += markdown[i++];
    }
    return text;
}
}

std::size_t CountWords(std::string_view markdown)
{
    std::string text = StripMarkdownCode(markdown);
    std::size_t words = 0;
    bool inWord = false;
    auto it = text.begin();
    while (it != text.end())
    {
        uint32 c = 0;
        try
        {
            c = utf8::next(it, text.end());
        }
        catch (utf8::exception const&)
        {
            break;
        }

        if (IsCjk(c))
        {
            ++words;
            inWord = false;
        }
        else if (IsWordCharacter(c))
        {
            if (!inWord)
                ++words;
            inWord = true;
        }
        else
            inWord = false;
    }
    return words;
}

bool IsValidUsername(std::string_view username)
{
    return username.size() >= 3 && username.size() <= 20 && std::ranges::all_of(username, [](char c)
    {
        return IsAsciiLowerOrDigit(c) || (c >= 'A' && c <= 'Z') || c == '_';
    });
}

bool IsValidEmail(std::string_view email)
{
    // Not RFC 5322: one @, something on both sides, a dot in the domain, no spaces or controls. The
    // verification mail is the real check
    if (email.size() < 3 || email.size() > 254 || !IsCleanText(email, false) || email.find(' ') != std::string_view::npos)
        return false;

    std::size_t at = email.find('@');
    if (at == std::string_view::npos || at == 0 || email.find('@', at + 1) != std::string_view::npos)
        return false;

    std::string_view domain = email.substr(at + 1);
    std::size_t dot = domain.find('.');
    return dot != std::string_view::npos && dot != 0 && !domain.ends_with('.') && domain.find("..") == std::string_view::npos;
}

bool IsValidSlug(std::string_view slug, std::size_t maxLength)
{
    if (slug.empty() || slug.size() > maxLength || slug.front() == '-' || slug.back() == '-' || slug.find("--") != std::string_view::npos)
        return false;

    return std::ranges::all_of(slug, [](char c) { return IsAsciiLowerOrDigit(c) || c == '-'; });
}

void FieldChecks::AddError(std::string_view key, std::string_view code)
{
    std::string name = _prefix.empty() ? std::string(key) : _prefix + "." + std::string(key);
    if (!_errors.contains(name))
        _errors[name] = code;
}

std::string FieldChecks::CheckText(std::string_view key, Optional<std::string_view> value, std::size_t minLength, std::size_t maxLength,
    bool required, bool multiLine)
{
    if (!value)
        return {};

    std::string_view text = Trim(*value);
    if (!IsCleanText(text, multiLine))
    {
        AddError(key, "invalid");
        return {};
    }

    std::size_t length = CountCharacters(text);
    if (text.empty() && !required)
        return {};

    if (length < minLength)
        AddError(key, text.empty() ? "required" : "too_short");
    else if (length > maxLength)
        AddError(key, "too_long");

    return std::string(text);
}

std::string FieldChecks::CheckUsername(std::string_view key, Optional<std::string_view> value)
{
    std::string username = CheckText(key, value, 3, 20, true, false);
    if (!username.empty() && !IsValidUsername(username))
        AddError(key, "invalid");

    return username;
}

std::string FieldChecks::CheckEmail(std::string_view key, Optional<std::string_view> value)
{
    std::string email = CheckText(key, value, 3, 254, true, false);
    if (!email.empty() && !IsValidEmail(email))
        AddError(key, "invalid");

    return email;
}

std::string FieldChecks::CheckPassword(std::string_view key, Optional<std::string_view> value)
{
    if (!value)
        return {};

    std::size_t length = CountCharacters(*value);
    if (length < 8)
        AddError(key, "too_short");
    else if (length > 128)
        AddError(key, "too_long");

    return std::string(*value);
}

std::string FieldChecks::CheckSlug(std::string_view key, Optional<std::string_view> value, std::size_t maxLength)
{
    std::string slug = CheckText(key, value, 1, maxLength, true, false);
    if (!slug.empty() && !IsValidSlug(slug, maxLength))
        AddError(key, "invalid");

    return slug;
}

std::string FieldChecks::CheckChoice(std::string_view key, Optional<std::string_view> value, std::vector<std::string_view> const& allowed, bool /*required*/)
{
    if (!value)
        return {};

    if (std::ranges::find(allowed, *value) == allowed.end())
    {
        AddError(key, "invalid");
        return {};
    }

    return std::string(*value);
}

std::vector<std::string> FieldChecks::CheckSlugList(std::string_view key, std::vector<Optional<std::string_view>> const& items, std::size_t maxCount,
    std::size_t maxLength)
{
    std::vector<std::string> slugs;
    if (items.size() > maxCount)
    {
        AddError(key, "too_long");
        return slugs;
    }

    for (Optional<std::string_view> const& item : items)
    {
        if (!item || !IsValidSlug(*item, maxLength))
        {
            AddError(key, "invalid");
            return {};
        }

        std::string slug(*item);
        if (std::ranges::find(slugs, slug) == slugs.end())
            slugs.push_back(std::move(slug));
    }

    return slugs;
}

boost::json::value const* RequestFields::Get(std::string_view key, bool required)
{
    boost::json::value const* value = _body.if_contains(key);
    if ((!value || value->is_null()) && required)
        AddError(key, "required");

    return value && !value->is_null() ? value : nullptr;
}

Optional<std::string_view> RequestFields::GetString(std::string_view key, bool required)
{
    boost::json::value const* value = Get(key, required);
    if (!value)
        return {};

    if (!value->is_string())
    {
        AddError(key, "invalid_type");
        return {};
    }

    return std::string_view(value->get_string());
}

std::string RequestFields::Text(std::string_view key, std::size_t minLength, std::size_t maxLength, bool required, bool multiLine)
{
    return CheckText(key, GetString(key, required), minLength, maxLength, required, multiLine);
}

std::string RequestFields::Username(std::string_view key)
{
    return CheckUsername(key, GetString(key, true));
}

std::string RequestFields::Email(std::string_view key)
{
    return CheckEmail(key, GetString(key, true));
}

std::string RequestFields::Password(std::string_view key)
{
    return CheckPassword(key, GetString(key, true));
}

std::string RequestFields::Lang(std::string_view key, bool required)
{
    return Choice(key, { "zh", "en" }, required);
}

std::string RequestFields::Slug(std::string_view key, std::size_t maxLength)
{
    return CheckSlug(key, GetString(key, true), maxLength);
}

std::string RequestFields::Choice(std::string_view key, std::vector<std::string_view> const& allowed, bool required)
{
    return CheckChoice(key, GetString(key, required), allowed, required);
}

Optional<bool> RequestFields::Bool(std::string_view key)
{
    boost::json::value const* value = Get(key, false);
    if (!value)
        return {};

    if (!value->is_bool())
    {
        AddError(key, "invalid_type");
        return {};
    }

    return value->get_bool();
}

std::vector<std::string> RequestFields::SlugList(std::string_view key, std::size_t maxCount, std::size_t maxLength)
{
    boost::json::value const* value = Get(key, false);
    if (!value)
        return {};

    boost::json::array const* array = value->if_array();
    if (!array)
    {
        AddError(key, "invalid_type");
        return {};
    }

    std::vector<Optional<std::string_view>> items;
    for (boost::json::value const& item : *array)
        items.push_back(item.is_string() ? Optional<std::string_view>(item.get_string()) : Optional<std::string_view>());
    return CheckSlugList(key, items, maxCount, maxLength);
}

boost::json::object const* RequestFields::Object(std::string_view key, bool required)
{
    boost::json::value const* value = Get(key, required);
    if (!value)
        return nullptr;

    if (!value->is_object())
    {
        AddError(key, "invalid_type");
        return nullptr;
    }

    return &value->get_object();
}

TableFields::TableFields(FieldErrors const& errors)
{
    for (auto const& [field, code] : errors)
        AddError(field, code);
}

namespace
{
Optional<std::string_view> Sent(flatbuffers::String const* value)
{
    return value ? Optional<std::string_view>(value->string_view()) : Optional<std::string_view>();
}
}

std::string TableFields::Text(std::string_view key, flatbuffers::String const* value, std::size_t minLength, std::size_t maxLength, bool required,
    bool multiLine)
{
    if (!value && required)
        AddError(key, "required");
    return CheckText(key, Sent(value), minLength, maxLength, required, multiLine);
}

std::string TableFields::Username(std::string_view key, flatbuffers::String const* value)
{
    if (!value)
        AddError(key, "required");
    return CheckUsername(key, Sent(value));
}

std::string TableFields::Email(std::string_view key, flatbuffers::String const* value)
{
    if (!value)
        AddError(key, "required");
    return CheckEmail(key, Sent(value));
}

std::string TableFields::Password(std::string_view key, flatbuffers::String const* value)
{
    if (!value)
        AddError(key, "required");
    return CheckPassword(key, Sent(value));
}

std::string TableFields::Lang(std::string_view key, flatbuffers::String const* value, bool required)
{
    return Choice(key, value, { "zh", "en" }, required);
}

std::string TableFields::Slug(std::string_view key, flatbuffers::String const* value, std::size_t maxLength)
{
    if (!value)
        AddError(key, "required");
    return CheckSlug(key, Sent(value), maxLength);
}

std::string TableFields::Choice(std::string_view key, flatbuffers::String const* value, std::vector<std::string_view> const& allowed, bool required)
{
    if (!value && required)
        AddError(key, "required");
    return CheckChoice(key, Sent(value), allowed, required);
}

std::vector<std::string> TableFields::SlugList(std::string_view key, flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> const* value,
    std::size_t maxCount, std::size_t maxLength)
{
    if (!value)
        return {};

    std::vector<Optional<std::string_view>> items;
    for (flatbuffers::String const* item : *value)
        items.push_back(item->string_view());
    return CheckSlugList(key, items, maxCount, maxLength);
}
}
