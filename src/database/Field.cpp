/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 */

#include "Field.h"
#include "Log.h"
#include <charconv>
#include <cstdio>
#include <limits>

namespace
{
template<typename T>
T ParseNumber(char const* value, uint32 length)
{
    if (!value)
        return T();

    // A boolean expression or column read as a number
    if (length == 1 && (*value == 't' || *value == 'f'))
        return T(*value == 't' ? 1 : 0);

    T result{};
    std::from_chars_result parsed = std::from_chars(value, value + length, result);
    if (parsed.ec != std::errc())
        RT_LOG_ERROR("sql.sql", "Field: \"{}\" isn't a value of this type", std::string_view(value, length));
    return result;
}
}

uint8 Field::GetUInt8() const noexcept { return ParseNumber<uint8>(_value, _length); }
int8 Field::GetInt8() const noexcept { return ParseNumber<int8>(_value, _length); }
uint16 Field::GetUInt16() const noexcept { return ParseNumber<uint16>(_value, _length); }
int16 Field::GetInt16() const noexcept { return ParseNumber<int16>(_value, _length); }
uint32 Field::GetUInt32() const noexcept { return ParseNumber<uint32>(_value, _length); }
int32 Field::GetInt32() const noexcept { return ParseNumber<int32>(_value, _length); }
uint64 Field::GetUInt64() const noexcept { return ParseNumber<uint64>(_value, _length); }
int64 Field::GetInt64() const noexcept { return ParseNumber<int64>(_value, _length); }
float Field::GetFloat() const noexcept { return ParseNumber<float>(_value, _length); }
double Field::GetDouble() const noexcept { return ParseNumber<double>(_value, _length); }

SystemTimePoint Field::GetDate() const noexcept
{
    if (!_value)
        return SystemTimePoint();

    int year = 0;
    unsigned month = 0, day = 0, hour = 0, minute = 0, second = 0;
    int const read = std::sscanf(_value, "%d-%u-%u %u:%u:%u", &year, &month, &day, &hour, &minute, &second);
    if (read != 3 && read != 6)
    {
        RT_LOG_ERROR("sql.sql", "Field: \"{}\" isn't a date", GetStringView());
        return SystemTimePoint();
    }

    std::chrono::sys_days const days{ std::chrono::year(year) / std::chrono::month(month) / std::chrono::day(day) };
    return std::chrono::time_point_cast<SystemTimePoint::duration>(days + std::chrono::hours(hour) + std::chrono::minutes(minute) + std::chrono::seconds(second));
}

std::vector<uint8> Field::GetBinary() const
{
    std::span<uint8 const> view = GetBinaryView();
    return { view.begin(), view.end() };
}

std::span<uint8 const> Field::GetBinaryView() const noexcept
{
    return _value ? std::span<uint8 const>(reinterpret_cast<uint8 const*>(_value), _length) : std::span<uint8 const>();
}
