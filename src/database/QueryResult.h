/*
 * This file is part of Rooftree, under the GNU GPL, version 2 or later: see COPYING.
 *
 * The rows of a query. A query without rows answers an empty pointer, not an empty ResultSet, so
 *
 *   if (QueryResult rows = Database.Query(stmt))
 *       do { Field* fields = rows->Fetch(); ... } while (rows->NextRow());
 *
 * Fields point into the result: a Field* is good until NextRow(), the copies its getters give for good.
 */

#ifndef ROOFTREE_DATABASE_QUERY_RESULT_H
#define ROOFTREE_DATABASE_QUERY_RESULT_H

#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Field.h"
#include <memory>
#include <string_view>
#include <vector>

typedef struct pg_result PGresult;

class ResultSet
{
public:
    // Takes the result, which has at least a row
    explicit ResultSet(PGresult* result);
    ResultSet(ResultSet const&) = delete;
    ResultSet& operator=(ResultSet const&) = delete;
    ~ResultSet();

    bool NextRow();
    uint64 GetRowCount() const { return _rowCount; }
    uint32 GetFieldCount() const { return uint32(_fields.size()); }
    Field* Fetch() { return _fields.data(); }
    Field const& operator[](std::size_t index) const;
    // By its name or alias in the SELECT
    Field const& operator[](std::string_view name) const;

private:
    void LoadRow();

    struct PQFree { void operator()(unsigned char* memory) const; };

    PGresult* _result;
    uint64 _rowCount;
    uint64 _row = 0;
    std::vector<Field> _fields;
    std::vector<bool> _isBinary;                                // bytea columns, decoded row by row
    std::vector<std::unique_ptr<unsigned char, PQFree>> _decoded;
};

#endif // ROOFTREE_DATABASE_QUERY_RESULT_H
