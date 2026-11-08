/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * FlatBuffers to JSON and back by a schema read at run time (its binary form, the
 * _bfbs_generated.h flatc writes with --bfbs-gen-embed), for front ends that speak JSON to handlers
 * that work in FlatBuffers: the request's JSON becomes the table the handler reads, the table it
 * answers with becomes the JSON the client expects. One schema describes both, nothing is written
 * per message.
 *
 * Tables, strings, scalars, enums (by name in JSON) and vectors of them; structs and unions aren't
 * supported (Load says which field has one). Field names are the JSON keys as they are.
 *
 *  JSON to a   keys the schema doesn't have are left out, so are nulls; a value of the wrong type or
 *  table       out of its type's range is left out too and named in the errors ("posts.0.id",
 *              "out_of_range"), so one request gets told of all of them. Every scalar the JSON has is
 *              stored, its default too, so an optional one (= null) tells "not sent" from 0
 *  a table to  every field is in the object, what isn't in the buffer as JSON says nothing: "" for a
 *  JSON        string, [] for a vector, null for a table or an optional scalar, the default for
 *              another scalar
 *
 * Four attributes, which a schema using them declares (attribute "json_null";):
 *
 *  json_null   null in JSON is the field empty: a null becomes "" / an empty table / [] / 0, and an
 *              empty, 0 or absent one is null. How a request says "remove this", or "the root" of
 *              an id where 0 is none
 *  json_omit   absent from the buffer, absent from the JSON: an answer's fields that only some have
 *  json_closed an object whose keys must all be fields of its table, else unknown_key (still built):
 *              a map of languages, say, where any other key is a mistake
 *  json_map    a vector of tables with a string field "key" is an object of what they hold by key:
 *              the one other field's value, or an object of the others ({"/a.png": {"width": 1}})
 */

#ifndef ROOFTREE_FLAT_JSON_H
#define ROOFTREE_FLAT_JSON_H

#include "Define.h"
#include "Optional.h"
#include <boost/json/object.hpp>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reflection
{
struct Object;
struct Schema;
}

namespace Rooftree::Server
{
// (path, code): "translations.zh.title", "not_a_string"
using FlatJsonErrors = std::vector<std::pair<std::string, std::string>>;

class FlatJsonSchema
{
public:
    // bfbs, a binary schema (XBinarySchema::data()), outlives this
    explicit FlatJsonSchema(uint8 const* bfbs);

    // Once, before use: false with error naming what isn't supported
    bool Load(std::string& error);

    // "Namespace.Table"
    reflection::Object const* Find(std::string_view name) const;

    // Whether buffer is a valid one of object, before anything reads it
    bool Verify(reflection::Object const& object, std::span<uint8 const> buffer) const;

    // A verified buffer
    boost::json::object ToJson(reflection::Object const& object, std::span<uint8 const> buffer) const;

    // A finished buffer of object, without what didn't fit, which errors names
    std::string FromJson(reflection::Object const& object, boost::json::object const& json, FlatJsonErrors& errors) const;

    // The names of object's fields, deprecated ones aside
    std::vector<std::string_view> GetFieldNames(reflection::Object const& object) const;

    // text (of a URL's path or query) as the JSON value of the field name: a number for a number,
    // true/false for a bool, else the text. None if it isn't one, or there is no such field
    Optional<boost::json::value> FromText(reflection::Object const& object, std::string_view name, std::string_view text) const;

private:
    reflection::Schema const* _schema;
};
}

#endif // ROOFTREE_FLAT_JSON_H
