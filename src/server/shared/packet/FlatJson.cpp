/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "FlatJson.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include <flatbuffers/reflection.h>
#include <limits>

namespace Rooftree::Server
{
namespace
{
using reflection::BaseType;

bool IsScalar(BaseType type)
{
    return type >= reflection::Bool && type <= reflection::Double;
}

// What a json_null scalar's null is
boost::json::value const ZeroValue(0);

bool HasAttribute(reflection::Field const& field, std::string_view name)
{
    if (field.attributes())
        for (reflection::KeyValue const* attribute : *field.attributes())
            if (attribute->key()->string_view() == name)
                return true;
    return false;
}

std::string_view Name(flatbuffers::String const* text)
{
    return { text->data(), text->size() };
}

class Converter
{
public:
    explicit Converter(reflection::Schema const& schema) : _schema(schema) { }

    boost::json::object TableToJson(reflection::Object const& object, flatbuffers::Table const& table) const
    {
        boost::json::object json;
        for (reflection::Field const* field : *object.fields())
        {
            if (field->deprecated())
                continue;

            reflection::Type const& type = *field->type();
            std::string_view key = Name(field->name());
            bool nullIfEmpty = HasAttribute(*field, "json_null");
            bool present = table.CheckField(field->offset());
            if (!present && HasAttribute(*field, "json_omit"))
                continue;

            if (IsScalar(type.base_type()))
            {
                if ((field->optional() && !present) || (nullIfEmpty && flatbuffers::GetAnyFieldI(table, *field) == 0))
                    json[key] = nullptr;
                else
                    json[key] = ScalarToJson(type.base_type(), type.index(), flatbuffers::GetAnyFieldI(table, *field),
                        flatbuffers::GetAnyFieldF(table, *field));
            }
            else if (type.base_type() == reflection::String)
            {
                flatbuffers::String const* text = flatbuffers::GetFieldS(table, *field);
                if (nullIfEmpty && (!text || !text->size()))
                    json[key] = nullptr;
                else
                    json[key] = text ? Name(text) : std::string_view();
            }
            else if (type.base_type() == reflection::Obj)
            {
                flatbuffers::Table const* child = flatbuffers::GetFieldT(table, *field);
                boost::json::object childJson = child ? TableToJson(*_schema.objects()->Get(type.index()), *child) : boost::json::object();
                if (!child || (nullIfEmpty && IsEmpty(*_schema.objects()->Get(type.index()), *child)))
                    json[key] = nullptr;
                else
                    json[key] = std::move(childJson);
            }
            else if (type.base_type() == reflection::Vector)
            {
                flatbuffers::VectorOfAny const* vector = flatbuffers::GetFieldAnyV(table, *field);
                if (nullIfEmpty && (!vector || !vector->size()))
                    json[key] = nullptr;
                else if (HasAttribute(*field, "json_map"))
                    json[key] = vector ? MapToJson(type, vector) : boost::json::object();
                else
                    json[key] = vector ? VectorToJson(type, vector) : boost::json::array();
            }
        }

        return json;
    }

    // A table of object built from json into builder. A field that doesn't fit is left out, its error
    // kept; nulls are left out but for json_null fields, which are then empty
    flatbuffers::uoffset_t BuildTable(reflection::Object const& object, boost::json::object const& json, flatbuffers::FlatBufferBuilder& builder)
    {
        // What tables hold by offset is built first, a table is built in one go
        std::vector<std::pair<reflection::Field const*, flatbuffers::uoffset_t>> offsets;
        std::vector<std::pair<reflection::Field const*, boost::json::value const*>> scalars;
        for (reflection::Field const* field : *object.fields())
        {
            boost::json::value const* value = json.if_contains(Name(field->name()));
            if (field->deprecated() || !value)
                continue;

            reflection::Type const& type = *field->type();
            PathScope scope(_path, Name(field->name()));
            if (value->is_null())
            {
                if (HasAttribute(*field, "json_null"))
                {
                    if (IsScalar(type.base_type()))
                        scalars.emplace_back(field, &ZeroValue);
                    else
                        offsets.emplace_back(field, BuildEmpty(type, builder));
                }
                continue;
            }

            if (IsScalar(type.base_type()))
                scalars.emplace_back(field, value);
            else if (HasAttribute(*field, "json_map"))
            {
                if (Optional<flatbuffers::uoffset_t> offset = BuildMap(type, *value, builder))
                    offsets.emplace_back(field, *offset);
            }
            else if (Optional<flatbuffers::uoffset_t> offset = BuildValue(type.base_type(), type, *value, builder))
            {
                if (type.base_type() == reflection::Obj && value->is_object() && HasAttribute(*field, "json_closed")
                    && !HasOnlyFields(*_schema.objects()->Get(type.index()), value->get_object()))
                    Fail("unknown_key");
                offsets.emplace_back(field, *offset);
            }
        }

        // A scalar is read before anything is added for it: one that doesn't fit leaves the table as it was
        std::vector<std::tuple<reflection::Field const*, Optional<int64>, double>> values;
        for (auto const& [field, value] : scalars)
        {
            PathScope scope(_path, Name(field->name()));
            Optional<int64> integer;
            double floating = 0;
            if (ReadScalar(field->type()->base_type(), field->type()->index(), *value, integer, floating))
                values.emplace_back(field, integer, floating);
        }

        flatbuffers::uoffset_t start = builder.StartTable();
        for (auto const& [field, integer, floating] : values)
            AddScalar(builder, *field, integer, floating);
        for (auto const& [field, offset] : offsets)
            builder.AddOffset(field->offset(), flatbuffers::Offset<void>(offset));
        return builder.EndTable(start);
    }

    FlatJsonErrors TakeErrors() { return std::move(_errors); }

    // The text of a URL as a value of a scalar or string field
    static Optional<boost::json::value> FromText(reflection::Field const& field, std::string_view text)
    {
        BaseType type = field.type()->base_type();
        if (type == reflection::String)
            return boost::json::value(text);
        if (type == reflection::Bool)
        {
            if (text == "true" || text == "false")
                return boost::json::value(text == "true");
            return {};
        }
        if (type == reflection::Float || type == reflection::Double)
        {
            if (Optional<double> number = Rooftree::StringTo<double>(text))
                return boost::json::value(*number);
            return {};
        }
        if (IsScalar(type))
        {
            if (Optional<int64> number = Rooftree::StringTo<int64>(text))
                return boost::json::value(*number);
            if (Optional<uint64> number = Rooftree::StringTo<uint64>(text))
                return boost::json::value(*number);
        }
        return {};
    }


private:
    // Where in the JSON the converter is, for errors
    struct PathScope
    {
        PathScope(std::string& path, std::string_view key) : _path(path), _size(path.size())
        {
            if (!_path.empty())
                _path += '.';
            _path += key;
        }
        ~PathScope() { _path.resize(_size); }

        std::string& _path;
        std::size_t _size;
    };

    boost::json::value ScalarToJson(BaseType type, int32 enumIndex, int64 integer, double floating) const
    {
        if (type == reflection::Bool)
            return integer != 0;
        if (type == reflection::Float || type == reflection::Double)
            return floating;
        if (enumIndex >= 0)
            if (reflection::EnumVal const* value = FindEnumValue(enumIndex, integer))
                return boost::json::value(Name(value->name()));
        if (type == reflection::ULong && integer < 0)
            return uint64(integer);
        return integer;
    }

    reflection::EnumVal const* FindEnumValue(int32 enumIndex, int64 integer) const
    {
        for (reflection::EnumVal const* value : *_schema.enums()->Get(enumIndex)->values())
            if (value->value() == integer)
                return value;
        return nullptr;
    }

    boost::json::array VectorToJson(reflection::Type const& type, flatbuffers::VectorOfAny const* vector) const
    {
        boost::json::array json;
        BaseType element = type.element();
        for (flatbuffers::uoffset_t i = 0; i < vector->size(); ++i)
        {
            if (IsScalar(element))
                json.push_back(ScalarToJson(element, type.index(), flatbuffers::GetAnyVectorElemI(vector, element, i),
                    flatbuffers::GetAnyVectorElemF(vector, element, i)));
            else if (element == reflection::String)
                json.push_back(boost::json::value(Name(flatbuffers::GetAnyVectorElemPointer<flatbuffers::String const>(vector, i))));
            else if (element == reflection::Obj)
                json.push_back(TableToJson(*_schema.objects()->Get(type.index()),
                    *flatbuffers::GetAnyVectorElemPointer<flatbuffers::Table const>(vector, i)));
        }

        return json;
    }

    // A json_map's entries as an object: { key: the one other field's value, or the other fields }
    boost::json::object MapToJson(reflection::Type const& type, flatbuffers::VectorOfAny const* vector) const
    {
        reflection::Object const& entry = *_schema.objects()->Get(type.index());
        reflection::Field const& keyField = *entry.fields()->LookupByKey("key");
        reflection::Field const* valueField = GetMapValueField(entry);
        boost::json::object json;
        for (flatbuffers::uoffset_t i = 0; i < vector->size(); ++i)
        {
            flatbuffers::Table const& table = *flatbuffers::GetAnyVectorElemPointer<flatbuffers::Table const>(vector, i);
            flatbuffers::String const* key = flatbuffers::GetFieldS(table, keyField);
            boost::json::object fields = TableToJson(entry, table);
            fields.erase("key");
            if (valueField)
                json[key ? Name(key) : std::string_view()] = std::move(fields[Name(valueField->name())]);
            else
                json[key ? Name(key) : std::string_view()] = std::move(fields);
        }

        return json;
    }

    void Fail(std::string_view code)
    {
        _errors.emplace_back(_path, code);
    }

    // Whether a table has no field set
    static bool IsEmpty(reflection::Object const& object, flatbuffers::Table const& table)
    {
        for (reflection::Field const* field : *object.fields())
            if (table.CheckField(field->offset()))
                return false;
        return true;
    }

    static bool HasOnlyFields(reflection::Object const& object, boost::json::object const& json)
    {
        for (auto const& [key, value] : json)
            if (!object.fields()->LookupByKey(std::string(key).c_str()))
                return false;
        return true;
    }

    // Of a json_map's entry: its one field but the key, null when it has several
    static reflection::Field const* GetMapValueField(reflection::Object const& entry)
    {
        reflection::Field const* value = nullptr;
        for (reflection::Field const* field : *entry.fields())
        {
            if (field->deprecated() || Name(field->name()) == "key")
                continue;
            if (value)
                return nullptr;
            value = field;
        }
        return value;
    }

    // A json_map's object as its entries, in the object's order; all of them or none
    Optional<flatbuffers::uoffset_t> BuildMap(reflection::Type const& type, boost::json::value const& value, flatbuffers::FlatBufferBuilder& builder)
    {
        if (!value.is_object())
            return Fail("not_an_object"), Optional<flatbuffers::uoffset_t>();

        reflection::Object const& entry = *_schema.objects()->Get(type.index());
        reflection::Field const* valueField = GetMapValueField(entry);
        std::size_t errors = _errors.size();
        std::vector<flatbuffers::uoffset_t> elements;
        for (auto const& [key, item] : value.get_object())
        {
            PathScope scope(_path, key);
            boost::json::object fields;
            if (valueField)
                fields[Name(valueField->name())] = item;
            else if (item.is_object())
                fields = item.get_object();
            else
            {
                Fail("not_an_object");
                continue;
            }
            fields["key"] = key;
            // A value's errors under its key, not under the field it is in
            std::size_t before = _errors.size();
            elements.push_back(BuildTable(entry, fields, builder));
            if (valueField)
                for (std::size_t i = before; i < _errors.size(); ++i)
                    _errors[i].first = _path;
        }
        if (_errors.size() != errors)
            return {};

        builder.StartVector(elements.size(), sizeof(flatbuffers::uoffset_t), sizeof(flatbuffers::uoffset_t));
        for (std::size_t i = elements.size(); i-- > 0;)
            builder.PushElement(flatbuffers::Offset<void>(elements[i]));
        return builder.EndVector(elements.size());
    }

    // A json_null field's null: "", an empty table, []
    flatbuffers::uoffset_t BuildEmpty(reflection::Type const& type, flatbuffers::FlatBufferBuilder& builder)
    {
        if (type.base_type() == reflection::String)
            return builder.CreateString("", 0).o;
        if (type.base_type() == reflection::Obj)
            return builder.EndTable(builder.StartTable());
        builder.StartVector(0, sizeof(flatbuffers::uoffset_t), sizeof(flatbuffers::uoffset_t));
        return builder.EndVector(0);
    }

    // A string, table or vector of type (baseType: the field's or, in a vector, the element's); none
    // with its error if value isn't one
    Optional<flatbuffers::uoffset_t> BuildValue(BaseType baseType, reflection::Type const& type, boost::json::value const& value,
        flatbuffers::FlatBufferBuilder& builder)
    {
        switch (baseType)
        {
            case reflection::String:
                if (!value.is_string())
                    return Fail("not_a_string"), Optional<flatbuffers::uoffset_t>();
                return builder.CreateString(value.get_string().data(), value.get_string().size()).o;
            case reflection::Obj:
                if (!value.is_object())
                    return Fail("not_an_object"), Optional<flatbuffers::uoffset_t>();
                return BuildTable(*_schema.objects()->Get(type.index()), value.get_object(), builder);
            case reflection::Vector:
                return BuildVector(type, value, builder);
            default:
                return Fail("unsupported"), Optional<flatbuffers::uoffset_t>();
        }
    }

    // All of it or none: an element that doesn't fit leaves the vector out
    Optional<flatbuffers::uoffset_t> BuildVector(reflection::Type const& type, boost::json::value const& value, flatbuffers::FlatBufferBuilder& builder)
    {
        if (!value.is_array())
            return Fail("not_an_array"), Optional<flatbuffers::uoffset_t>();

        boost::json::array const& array = value.get_array();
        BaseType element = type.element();
        std::size_t errors = _errors.size();
        if (IsScalar(element))
        {
            std::vector<std::pair<Optional<int64>, double>> values(array.size());
            for (std::size_t i = 0; i < array.size(); ++i)
            {
                PathScope scope(_path, std::to_string(i));
                ReadScalar(element, type.index(), array[i], values[i].first, values[i].second);
            }
            if (_errors.size() != errors)
                return {};

            std::size_t size = flatbuffers::GetTypeSize(element);
            builder.StartVector(array.size(), size, size);
            for (std::size_t i = array.size(); i-- > 0;)
                PushScalar(builder, element, values[i].first, values[i].second);
            return builder.EndVector(array.size());
        }

        std::vector<flatbuffers::uoffset_t> elements;
        for (std::size_t i = 0; i < array.size(); ++i)
        {
            PathScope scope(_path, std::to_string(i));
            if (array[i].is_null())
                Fail("null");
            else if (Optional<flatbuffers::uoffset_t> offset = BuildValue(element, type, array[i], builder))
                elements.push_back(*offset);
        }
        if (_errors.size() != errors)
            return {};

        builder.StartVector(elements.size(), sizeof(flatbuffers::uoffset_t), sizeof(flatbuffers::uoffset_t));
        for (std::size_t i = elements.size(); i-- > 0;)
            builder.PushElement(flatbuffers::Offset<void>(elements[i]));
        return builder.EndVector(elements.size());
    }

    // value as type: integer for Bool and the integers (enums by name too), floating for Float, Double
    bool ReadScalar(BaseType type, int32 enumIndex, boost::json::value const& value, Optional<int64>& integer, double& floating)
    {
        if (type == reflection::Bool)
        {
            if (!value.is_bool())
                return Fail("not_a_bool"), false;
            integer = value.get_bool() ? 1 : 0;
            return true;
        }

        if (type == reflection::Float || type == reflection::Double)
        {
            if (!value.is_number())
                return Fail("not_a_number"), false;
            floating = value.to_number<double>();
            return true;
        }

        if (enumIndex >= 0 && value.is_string())
        {
            for (reflection::EnumVal const* enumValue : *_schema.enums()->Get(enumIndex)->values())
                if (Name(enumValue->name()) == value.get_string())
                    return integer = enumValue->value(), true;
            return Fail("unknown_value"), false;
        }

        // The type's range, ULong's beyond int64 as uint64
        if (type == reflection::ULong && value.is_uint64())
            return integer = int64(value.get_uint64()), true;

        // A double isn't one, even 2.0: what writes it means a fraction. An unsigned C++ value is a
        // uint64 in Boost.JSON
        bool small = value.is_uint64() && value.get_uint64() <= uint64(std::numeric_limits<int64>::max());
        if (!value.is_int64() && !small)
            return Fail(value.is_number() ? "out_of_range" : "not_an_integer"), false;

        int64 number = value.is_int64() ? value.get_int64() : int64(value.get_uint64());
        auto inRange = [&]<typename T>(T)
        {
            return number >= int64(std::numeric_limits<T>::min()) && number <= int64(std::numeric_limits<T>::max());
        };
        bool fits = false;
        switch (type)
        {
            case reflection::Byte: fits = inRange(int8()); break;
            case reflection::UByte: fits = inRange(uint8()); break;
            case reflection::Short: fits = inRange(int16()); break;
            case reflection::UShort: fits = inRange(uint16()); break;
            case reflection::Int: fits = inRange(int32()); break;
            case reflection::UInt: fits = inRange(uint32()); break;
            case reflection::Long: fits = true; break;
            case reflection::ULong: fits = number >= 0; break;
            default: break;
        }
        if (!fits)
            return Fail("out_of_range"), false;
        integer = number;
        return true;
    }

    static void PushScalar(flatbuffers::FlatBufferBuilder& builder, BaseType type, Optional<int64> integer, double floating)
    {
        switch (type)
        {
            case reflection::Bool: builder.PushElement(uint8(*integer)); break;
            case reflection::Byte: builder.PushElement(int8(*integer)); break;
            case reflection::UByte: builder.PushElement(uint8(*integer)); break;
            case reflection::Short: builder.PushElement(int16(*integer)); break;
            case reflection::UShort: builder.PushElement(uint16(*integer)); break;
            case reflection::Int: builder.PushElement(int32(*integer)); break;
            case reflection::UInt: builder.PushElement(uint32(*integer)); break;
            case reflection::Long: builder.PushElement(int64(*integer)); break;
            case reflection::ULong: builder.PushElement(uint64(*integer)); break;
            case reflection::Float: builder.PushElement(float(floating)); break;
            case reflection::Double: builder.PushElement(floating); break;
            default: break;
        }
    }

    // Stored even when it is the default: what the JSON had, an optional one present
    static void AddScalar(flatbuffers::FlatBufferBuilder& builder, reflection::Field const& field, Optional<int64> integer, double floating)
    {
        reflection::Type const& type = *field.type();
        flatbuffers::voffset_t offset = field.offset();
        switch (type.base_type())
        {
            case reflection::Bool: builder.AddElement(offset, uint8(*integer)); break;
            case reflection::Byte: builder.AddElement(offset, int8(*integer)); break;
            case reflection::UByte: builder.AddElement(offset, uint8(*integer)); break;
            case reflection::Short: builder.AddElement(offset, int16(*integer)); break;
            case reflection::UShort: builder.AddElement(offset, uint16(*integer)); break;
            case reflection::Int: builder.AddElement(offset, int32(*integer)); break;
            case reflection::UInt: builder.AddElement(offset, uint32(*integer)); break;
            case reflection::Long: builder.AddElement(offset, int64(*integer)); break;
            case reflection::ULong: builder.AddElement(offset, uint64(*integer)); break;
            case reflection::Float: builder.AddElement(offset, float(floating)); break;
            case reflection::Double: builder.AddElement(offset, floating); break;
            default: break;
        }
    }

    reflection::Schema const& _schema;
    std::string _path;
    FlatJsonErrors _errors;
};
}

FlatJsonSchema::FlatJsonSchema(uint8 const* bfbs) : _schema(reflection::GetSchema(bfbs))
{
}

bool FlatJsonSchema::Load(std::string& error)
{
    auto supported = [&](BaseType type, int32 index)
    {
        if (type == reflection::Union || type == reflection::UType || type == reflection::Array || type == reflection::Vector64)
            return false;
        return type != reflection::Obj || !_schema->objects()->Get(index)->is_struct();
    };

    for (reflection::Object const* object : *_schema->objects())
    {
        if (object->is_struct())
            continue;
        for (reflection::Field const* field : *object->fields())
        {
            reflection::Type const& type = *field->type();
            bool ok = supported(type.base_type(), type.index())
                && (type.base_type() != reflection::Vector || (supported(type.element(), type.index()) && type.element() != reflection::Vector));
            if (!ok)
            {
                error = StringFormat("{}.{}: structs, unions, arrays and vectors of vectors aren't supported", Name(object->name()), Name(field->name()));
                return false;
            }

            if (HasAttribute(*field, "json_map"))
            {
                reflection::Field const* key = type.base_type() == reflection::Vector && type.element() == reflection::Obj
                    ? _schema->objects()->Get(type.index())->fields()->LookupByKey("key") : nullptr;
                if (!key || key->type()->base_type() != reflection::String)
                {
                    error = StringFormat("{}.{}: a json_map is a vector of tables with a string key", Name(object->name()), Name(field->name()));
                    return false;
                }
            }
        }
    }

    return true;
}

reflection::Object const* FlatJsonSchema::Find(std::string_view name) const
{
    for (reflection::Object const* object : *_schema->objects())
        if (Name(object->name()) == name)
            return object;
    return nullptr;
}

bool FlatJsonSchema::Verify(reflection::Object const& object, std::span<uint8 const> buffer) const
{
    return flatbuffers::Verify(*_schema, object, buffer.data(), buffer.size());
}

boost::json::object FlatJsonSchema::ToJson(reflection::Object const& object, std::span<uint8 const> buffer) const
{
    return Converter(*_schema).TableToJson(object, *flatbuffers::GetAnyRoot(buffer.data()));
}

std::string FlatJsonSchema::FromJson(reflection::Object const& object, boost::json::object const& json, FlatJsonErrors& errors) const
{
    flatbuffers::FlatBufferBuilder builder;
    Converter converter(*_schema);
    builder.Finish(flatbuffers::Offset<flatbuffers::Table>(converter.BuildTable(object, json, builder)));
    errors = converter.TakeErrors();
    return std::string(reinterpret_cast<char const*>(builder.GetBufferPointer()), builder.GetSize());
}

std::vector<std::string_view> FlatJsonSchema::GetFieldNames(reflection::Object const& object) const
{
    std::vector<std::string_view> names;
    for (reflection::Field const* field : *object.fields())
        if (!field->deprecated())
            names.push_back(Name(field->name()));
    return names;
}

Optional<boost::json::value> FlatJsonSchema::FromText(reflection::Object const& object, std::string_view name, std::string_view text) const
{
    for (reflection::Field const* field : *object.fields())
        if (!field->deprecated() && Name(field->name()) == name)
            return Converter::FromText(*field, text);
    return {};
}
}
