/*
 * This file is part of Rooftree, under the GNU GPL, version 3 or later: see COPYING.
 *
 * A prepared statement waiting to run: which one, and its parameters by position. It isn't tied to a
 * connection; any connection of the pool has the statement prepared. All parameters go as text, which
 * PostgreSQL parses as the type it inferred for that place, bytes as binary.
 */

#ifndef ROOFTREE_DATABASE_PREPARED_STATEMENT_H
#define ROOFTREE_DATABASE_PREPARED_STATEMENT_H

#include "Define.h"
#include "Duration.h"
#include "Optional.h"
#include <span>
#include <string>
#include <string_view>
#include <vector>

class PreparedStatementBase
{
public:
    PreparedStatementBase(uint32 index, uint8 parameterCount);
    PreparedStatementBase(PreparedStatementBase const&) = delete;
    PreparedStatementBase& operator=(PreparedStatementBase const&) = delete;
    virtual ~PreparedStatementBase();

    void setNull(uint8 index);
    void setBool(uint8 index, bool value);
    void setUInt8(uint8 index, uint8 value);
    void setUInt16(uint8 index, uint16 value);
    void setUInt32(uint8 index, uint32 value);
    void setUInt64(uint8 index, uint64 value);
    void setInt8(uint8 index, int8 value);
    void setInt16(uint8 index, int16 value);
    void setInt32(uint8 index, int32 value);
    void setInt64(uint8 index, int64 value);
    void setFloat(uint8 index, float value);
    void setDouble(uint8 index, double value);
    void setDate(uint8 index, SystemTimePoint value);
    void setString(uint8 index, std::string_view value);
    void setBinary(uint8 index, std::span<uint8 const> value);

    uint32 GetIndex() const { return _index; }

    struct Parameter
    {
        Optional<std::string> Value;    // nothing: NULL
        bool IsBinary = false;
    };

    std::vector<Parameter> const& GetParameters() const { return _parameters; }

private:
    Parameter& At(uint8 index);

    uint32 _index;
    std::vector<Parameter> _parameters;
};

template<typename T>
class PreparedStatement : public PreparedStatementBase
{
public:
    using PreparedStatementBase::PreparedStatementBase;
};

#endif // ROOFTREE_DATABASE_PREPARED_STATEMENT_H
