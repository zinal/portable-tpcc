#pragma once

#include <pqxx/pqxx>

#include <initializer_list>
#include <string>
#include <vector>

// libpqxx 7.2 in contrib exposes params in pqxx::internal; newer releases
// moved it to pqxx::params. Provide the alias expected by tpcc-postgres-cpp.
namespace pqxx {
using params = internal::params;
} // namespace pqxx

inline pqxx::stream_to MakeCopyStream(
    pqxx::transaction_base& txn,
    std::string_view table,
    std::initializer_list<const char*> columns)
{
    std::vector<std::string> cols;
    cols.reserve(columns.size());
    for (const char* col : columns) {
        cols.emplace_back(col);
    }
    return pqxx::stream_to(txn, table, cols);
}
