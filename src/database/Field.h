/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * One value of a result row, as PostgreSQL sent it: text (bytea decoded). Each getter parses what it is
 * asked for, so the column only has to hold something of that kind: GetUInt32 reads an integer column of
 * any width, GetBool a boolean or a number. NULL reads as 0 or empty, the *OrNull ones tell it apart.
 */

#ifndef ROOFTREE_DATABASE_FIELD_H
#define ROOFTREE_DATABASE_FIELD_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

class Field
{
public:
    bool GetBool() const noexcept { return GetUInt8() != 0; }
    uint8 GetUInt8() const noexcept;
    int8 GetInt8() const noexcept;
    uint16 GetUInt16() const noexcept;
    int16 GetInt16() const noexcept;
    uint32 GetUInt32() const noexcept;
    int32 GetInt32() const noexcept;
    uint64 GetUInt64() const noexcept;
    int64 GetInt64() const noexcept;
    float GetFloat() const noexcept;
    double GetDouble() const noexcept;
    // A timestamp without time zone or a date, taken as UTC
    SystemTimePoint GetDate() const noexcept;
    char const* GetCString() const noexcept { return _value; }
    std::string GetString() const { return std::string(GetStringView()); }
    std::string_view GetStringView() const noexcept { return _value ? std::string_view(_value, _length) : std::string_view(); }
    std::vector<uint8> GetBinary() const;
    std::span<uint8 const> GetBinaryView() const noexcept;

    bool IsNull() const noexcept { return _value == nullptr; }

    Optional<uint8> GetUInt8OrNull() const noexcept { return IsNull() ? Optional<uint8>() : GetUInt8(); }
    Optional<int8> GetInt8OrNull() const noexcept { return IsNull() ? Optional<int8>() : GetInt8(); }
    Optional<uint16> GetUInt16OrNull() const noexcept { return IsNull() ? Optional<uint16>() : GetUInt16(); }
    Optional<int16> GetInt16OrNull() const noexcept { return IsNull() ? Optional<int16>() : GetInt16(); }
    Optional<uint32> GetUInt32OrNull() const noexcept { return IsNull() ? Optional<uint32>() : GetUInt32(); }
    Optional<int32> GetInt32OrNull() const noexcept { return IsNull() ? Optional<int32>() : GetInt32(); }
    Optional<uint64> GetUInt64OrNull() const noexcept { return IsNull() ? Optional<uint64>() : GetUInt64(); }
    Optional<int64> GetInt64OrNull() const noexcept { return IsNull() ? Optional<int64>() : GetInt64(); }
    Optional<float> GetFloatOrNull() const noexcept { return IsNull() ? Optional<float>() : GetFloat(); }
    Optional<double> GetDoubleOrNull() const noexcept { return IsNull() ? Optional<double>() : GetDouble(); }
    Optional<SystemTimePoint> GetDateOrNull() const noexcept { return IsNull() ? Optional<SystemTimePoint>() : GetDate(); }
    Optional<std::string> GetStringOrNull() const { return IsNull() ? Optional<std::string>() : GetString(); }
    Optional<std::string_view> GetStringViewOrNull() const noexcept { return IsNull() ? Optional<std::string_view>() : GetStringView(); }
    Optional<std::vector<uint8>> GetBinaryOrNull() const { return IsNull() ? Optional<std::vector<uint8>>() : GetBinary(); }
    Optional<std::span<uint8 const>> GetBinaryViewOrNull() const noexcept { return IsNull() ? Optional<std::span<uint8 const>>() : GetBinaryView(); }

private:
    friend class ResultSet;

    char const* _value = nullptr;
    uint32 _length = 0;
};

#endif // ROOFTREE_DATABASE_FIELD_H
