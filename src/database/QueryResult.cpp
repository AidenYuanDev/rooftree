/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 */

#include "QueryResult.h"
#include "Errors.h"
#include <libpq-fe.h>

namespace
{
constexpr Oid ByteaOid = 17;
}

void ResultSet::PQFree::operator()(unsigned char* memory) const
{
    PQfreemem(memory);
}

ResultSet::ResultSet(PGresult* result) : _result(result), _rowCount(PQntuples(result)), _fields(PQnfields(result)), _isBinary(_fields.size()), _decoded(_fields.size())
{
    for (std::size_t i = 0; i < _fields.size(); ++i)
        _isBinary[i] = PQftype(result, int(i)) == ByteaOid;

    LoadRow();
}

ResultSet::~ResultSet()
{
    PQclear(_result);
}

bool ResultSet::NextRow()
{
    if (++_row >= _rowCount)
        return false;

    LoadRow();
    return true;
}

void ResultSet::LoadRow()
{
    int const row = int(_row);
    for (std::size_t i = 0; i < _fields.size(); ++i)
    {
        int const column = int(i);
        Field& field = _fields[i];
        if (PQgetisnull(_result, row, column))
        {
            field._value = nullptr;
            field._length = 0;
        }
        else if (_isBinary[i])
        {
            std::size_t length = 0;
            _decoded[i].reset(PQunescapeBytea(reinterpret_cast<unsigned char const*>(PQgetvalue(_result, row, column)), &length));
            ASSERT(_decoded[i], "Out of memory decoding a bytea");
            field._value = reinterpret_cast<char const*>(_decoded[i].get());
            field._length = uint32(length);
        }
        else
        {
            field._value = PQgetvalue(_result, row, column);
            field._length = uint32(PQgetlength(_result, row, column));
        }
    }
}

Field const& ResultSet::operator[](std::size_t index) const
{
    ASSERT(index < _fields.size());
    return _fields[index];
}

Field const& ResultSet::operator[](std::string_view name) const
{
    int const column = PQfnumber(_result, std::string(name).c_str());
    ASSERT(column >= 0, "No column %s in the result", std::string(name).c_str());
    return _fields[std::size_t(column)];
}
