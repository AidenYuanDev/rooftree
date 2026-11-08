/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * Reading a JSON request body field by field, collecting one error code per bad field, so a single
 * 400 tells the form about every field at once:
 *
 *   RequestFields fields(*body);
 *   std::string name = fields.Text("displayName", 1, 40);
 *   std::string email = fields.Email("email");
 *   if (!fields.IsValid())
 *       return request->RespondError(400, "validation_failed", {}, fields.TakeErrors());
 *
 * Error codes (the frontend translates them): required, invalid_type, too_short, too_long, invalid.
 * Lengths are counted in characters (code points), not bytes. TableFields does the same for the
 * FlatBuffers table of an admin operation (ApiCall.h).
 */

#ifndef BLOG_VALIDATION_H
#define BLOG_VALIDATION_H

#include "Define.h"
#include "Optional.h"
#include <boost/json/array.hpp>
#include <boost/json/object.hpp>
#include <flatbuffers/flatbuffers.h>
#include <string>
#include <string_view>
#include <vector>

namespace Blog
{
// Code points of valid UTF-8
std::size_t CountCharacters(std::string_view text);

// What a reader sees of a text whatever the Markdown around it, to find a passage selected on a page
// in a post's Markdown: its letters, digits and other characters, lower case. No whitespace, no
// punctuation (ASCII, CJK, typographic: emphasis, code marks, quotes made curly all go), no link
// targets. Find one's in another's with std::u32string::find
std::u32string MatchableText(std::string_view text);

// Trims, then refuses control characters (but \n and \t when multiLine)
bool IsCleanText(std::string_view text, bool multiLine);

// Kana, CJK ideographs and Hangul: each one is a word
bool IsCjk(uint32 c);
// Inside a word: ASCII letters and digits, and letters of other scripts. Spaces, ASCII and general
// punctuation, CJK and full width punctuation, emoji end one
bool IsWordCharacter(uint32 c);

// Length of a text as readers count it (the frontend's lib/text.ts does the same): each CJK
// character is a word, other words are runs of letters and digits. Code and link targets don't count
std::size_t CountWords(std::string_view markdown);

bool IsValidUsername(std::string_view username);       // 3 to 20 of [A-Za-z0-9_]
bool IsValidEmail(std::string_view email);
bool IsValidSlug(std::string_view slug, std::size_t maxLength);  // lower case letters and digits, single '-' between

// What a request's readers share: one error code per bad field, under a prefix for nested ones. The
// checks take a value as it was sent, none when it wasn't
class FieldChecks
{
public:
    void AddError(std::string_view key, std::string_view code);
    bool IsValid() const { return _errors.empty(); }
    boost::json::object TakeErrors() { return std::move(_errors); }

    // For nested objects: errors are reported as "prefix.key"
    void SetPrefix(std::string prefix) { _prefix = std::move(prefix); }

protected:
    std::string CheckText(std::string_view key, Optional<std::string_view> value, std::size_t minLength, std::size_t maxLength, bool required,
        bool multiLine);
    std::string CheckUsername(std::string_view key, Optional<std::string_view> value);
    std::string CheckEmail(std::string_view key, Optional<std::string_view> value);
    std::string CheckPassword(std::string_view key, Optional<std::string_view> value);
    std::string CheckSlug(std::string_view key, Optional<std::string_view> value, std::size_t maxLength);
    std::string CheckChoice(std::string_view key, Optional<std::string_view> value, std::vector<std::string_view> const& allowed, bool required);
    // items: none for one that isn't a string
    std::vector<std::string> CheckSlugList(std::string_view key, std::vector<Optional<std::string_view>> const& items, std::size_t maxCount,
        std::size_t maxLength);

private:
    boost::json::object _errors;
    std::string _prefix;
};

class RequestFields : public FieldChecks
{
public:
    explicit RequestFields(boost::json::object const& body) : _body(body) { }

    bool Has(std::string_view key) const { return _body.contains(key); }
    // The value as sent, for what no reader above takes; none when absent
    boost::json::value const* Find(std::string_view key) const { return _body.if_contains(key); }

    // A trimmed string of minLength to maxLength characters; "" when missing and optional
    std::string Text(std::string_view key, std::size_t minLength, std::size_t maxLength, bool required = true, bool multiLine = false);

    std::string Username(std::string_view key);
    std::string Email(std::string_view key);
    // Passwords are taken as they are, not trimmed
    std::string Password(std::string_view key);
    std::string Lang(std::string_view key, bool required = true);
    std::string Slug(std::string_view key, std::size_t maxLength);
    // One of allowed
    std::string Choice(std::string_view key, std::vector<std::string_view> const& allowed, bool required = true);
    Optional<bool> Bool(std::string_view key);
    // A list of slugs
    std::vector<std::string> SlugList(std::string_view key, std::size_t maxCount, std::size_t maxLength);
    // A nested object, empty when missing (and not an error unless required)
    boost::json::object const* Object(std::string_view key, bool required = false);

private:
    // A string as sent; none when missing (an error if required) or not a string (invalid_type)
    Optional<std::string_view> GetString(std::string_view key, bool required);
    boost::json::value const* Get(std::string_view key, bool required);

    boost::json::object const& _body;
};

// (field, code), as FieldChecks names them: "translations.zh.title", "invalid_type"
using FieldErrors = std::vector<std::pair<std::string, std::string>>;

// RequestFields of a FlatBuffers table (ApiCall.h): the same checks and codes on the values a handler
// reads from it, a null pointer for a field that wasn't sent. Scalars need none, the table has their type
class TableFields : public FieldChecks
{
public:
    // Starting with what the JSON a table was made from had wrong, which wins over later errors
    explicit TableFields(FieldErrors const& errors = {});

    std::string Text(std::string_view key, flatbuffers::String const* value, std::size_t minLength, std::size_t maxLength, bool required = true,
        bool multiLine = false);
    std::string Username(std::string_view key, flatbuffers::String const* value);
    std::string Email(std::string_view key, flatbuffers::String const* value);
    std::string Password(std::string_view key, flatbuffers::String const* value);
    std::string Lang(std::string_view key, flatbuffers::String const* value, bool required = true);
    std::string Slug(std::string_view key, flatbuffers::String const* value, std::size_t maxLength);
    std::string Choice(std::string_view key, flatbuffers::String const* value, std::vector<std::string_view> const& allowed, bool required = true);
    std::vector<std::string> SlugList(std::string_view key, flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>> const* value,
        std::size_t maxCount, std::size_t maxLength);
};
}

#endif // BLOG_VALIDATION_H
