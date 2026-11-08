/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "PreparedStatement.h"
#include "Errors.h"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <fmt/format.h>
#include <limits>

PreparedStatementBase::PreparedStatementBase(uint32 index, uint8 parameterCount) : _index(index), _parameters(parameterCount)
{
}

PreparedStatementBase::~PreparedStatementBase() = default;

PreparedStatementBase::Parameter& PreparedStatementBase::At(uint8 index)
{
    ASSERT(index < _parameters.size(), "Statement %u has %zu parameters, set one at %u", _index, _parameters.size(), uint32(index));
    return _parameters[index];
}

void PreparedStatementBase::setNull(uint8 index) { At(index) = {}; }
// As a number: it is read as a boolean or an integer, whichever that place of the statement takes
void PreparedStatementBase::setBool(uint8 index, bool value) { At(index) = { value ? "1" : "0" }; }
void PreparedStatementBase::setUInt8(uint8 index, uint8 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setUInt16(uint8 index, uint16 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setUInt32(uint8 index, uint32 value) { At(index) = { std::to_string(value) }; }
// PostgreSQL has no unsigned types. A value past bigint is "after everything" (a cursor's default), that
// is what the largest bigint means too; as itself it would fail the whole statement
void PreparedStatementBase::setUInt64(uint8 index, uint64 value) { At(index) = { std::to_string(std::min<uint64>(value, std::numeric_limits<int64>::max())) }; }
void PreparedStatementBase::setInt8(uint8 index, int8 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setInt16(uint8 index, int16 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setInt32(uint8 index, int32 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setInt64(uint8 index, int64 value) { At(index) = { std::to_string(value) }; }
void PreparedStatementBase::setFloat(uint8 index, float value) { At(index) = { fmt::to_string(value) }; }
void PreparedStatementBase::setDouble(uint8 index, double value) { At(index) = { fmt::to_string(value) }; }

void PreparedStatementBase::setDate(uint8 index, SystemTimePoint value)
{
    std::chrono::sys_days const day = std::chrono::floor<std::chrono::days>(value);
    std::chrono::year_month_day const date(day);
    std::chrono::hh_mm_ss const time(std::chrono::floor<std::chrono::seconds>(value - day));
    At(index) = { fmt::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", int(date.year()), unsigned(date.month()), unsigned(date.day()),
        time.hours().count(), time.minutes().count(), time.seconds().count()) };
}

void PreparedStatementBase::setString(uint8 index, std::string_view value)
{
    // PostgreSQL's text can't hold a NUL, the only character it refuses in valid UTF-8
    std::string text(value);
    std::erase(text, '\0');
    At(index) = { std::move(text) };
}

void PreparedStatementBase::setBinary(uint8 index, std::span<uint8 const> value)
{
    At(index) = { .Value = std::string(reinterpret_cast<char const*>(value.data()), value.size()), .IsBinary = true };
}
