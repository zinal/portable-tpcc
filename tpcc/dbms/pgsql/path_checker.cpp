#include "path_checker.h"

#include <constants.h>
#include <log.h>
#include <domain_util.h>

#include <pqxx/pqxx>

#include <fmt/format.h>
#include <iostream>
#include <unordered_set>
#include <string>

namespace NTpcc {

namespace {

void SetSearchPath(pqxx::connection& conn, const std::string& path) {
    if (!path.empty()) {
        pqxx::nontransaction ntx(conn);
        ntx.exec(fmt::format("SET search_path TO {}", conn.quote_name(path)));
    }
}

std::unordered_set<std::string> ListTables(pqxx::connection& conn, const std::string& schema) {
    pqxx::nontransaction ntx(conn);
    auto result = ntx.exec_params(
        "SELECT table_name FROM information_schema.tables "
        "WHERE table_schema = $1 AND table_type IN ('BASE TABLE', 'PARTITIONED TABLE')",
        schema);

    std::unordered_set<std::string> tables;
    for (const auto& row : result) {
        tables.insert(row[0].as<std::string>());
    }
    return tables;
}

std::unordered_set<std::string> ListIndexes(pqxx::connection& conn, const std::string& schema,
                                            const std::string& tableName) {
    pqxx::nontransaction ntx(conn);
    auto result = ntx.exec_params(
        "SELECT indexname FROM pg_indexes "
        "WHERE schemaname = $1 AND tablename = $2",
        schema, tableName);

    std::unordered_set<std::string> indexes;
    for (const auto& row : result) {
        indexes.insert(row[0].as<std::string>());
    }
    return indexes;
}

void CheckTablesExist(pqxx::connection& conn, const std::string& schema, const char* what) {
    auto tables = ListTables(conn, schema);

    for (const char* table : TPCC_TABLES) {
        if (!tables.contains(table)) {
            std::cerr << "TPC-C table '" << table << "' is missing. " << what << std::endl;
            std::exit(1);
        }
    }
}

void CheckNoTablesExist(pqxx::connection& conn, const std::string& schema, const char* what) {
    auto tables = ListTables(conn, schema);

    for (const char* table : TPCC_TABLES) {
        if (tables.contains(table)) {
            std::cerr << "TPC-C table '" << table << "' already exists. " << what << std::endl;
            std::exit(1);
        }
    }
}

void CheckIndexExists(pqxx::connection& conn, const std::string& schema,
                      const std::string& tableName, const std::string& expectedIndex) {
    auto indexes = ListIndexes(conn, schema, tableName);
    if (!indexes.contains(expectedIndex)) {
        std::cerr << "Table '" << tableName
                  << "' is missing expected index '" << expectedIndex
                  << "'. Run 'tpcc indexes' after bulk load."
                  << std::endl;
        std::exit(1);
    }
}

int GetWarehouseCount(pqxx::connection& conn) {
    pqxx::nontransaction ntx(conn);
    auto row = ntx.exec("SELECT COUNT(*) FROM warehouse").front();
    return row[0].as<int>();
}

} // anonymous

void CheckDbForInit(const std::string& connectionString, const std::string& path) noexcept {
    try {
        pqxx::connection conn(connectionString);
        auto schema = GetEffectiveSchema(path);
        CheckNoTablesExist(conn, schema, "Already inited or forgot to drop?");
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for init failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

void CheckDbForImport(const std::string& connectionString, const std::string& path) noexcept {
    try {
        pqxx::connection conn(connectionString);
        auto schema = GetEffectiveSchema(path);
        SetSearchPath(conn, path);

        CheckTablesExist(conn, schema, "Run 'tpcc init' first.");

        // Idempotent import inserts assigned warehouse ranges; PK conflicts
        // trigger delete+reload for that warehouse. Non-empty tables are OK —
        // including when peer loaders have already started inserting.
        int whCount = GetWarehouseCount(conn);
        if (whCount != 0) {
            LOG_I("Database already has " << whCount
                  << " warehouses; concurrent loaders or a resumed run may observe this "
                     "(assigned ranges reload on key conflict)");
        }
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for import failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

void CheckDbForIndexes(const std::string& connectionString, const std::string& path) noexcept {
    try {
        pqxx::connection conn(connectionString);
        auto schema = GetEffectiveSchema(path);
        SetSearchPath(conn, path);
        CheckTablesExist(conn, schema, "Run 'tpcc init' and 'tpcc import' first.");
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for indexes failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

void CheckDbForRun(const std::string& connectionString, int expectedWhCount,
                   const std::string& path) noexcept {
    try {
        pqxx::connection conn(connectionString);
        auto schema = GetEffectiveSchema(path);
        SetSearchPath(conn, path);

        CheckTablesExist(conn, schema, "Run 'tpcc init' and 'tpcc import' first.");

        CheckIndexExists(conn, schema, TABLE_CUSTOMER, INDEX_CUSTOMER_NAME);
        CheckIndexExists(conn, schema, TABLE_OORDER, INDEX_ORDER);

        int whCount = GetWarehouseCount(conn);
        if (whCount == 0) {
            std::cerr << "Empty warehouse table (and maybe missing other TPC-C data), "
                      << "run 'tpcc import' first" << std::endl;
            std::exit(1);
        }
        if (whCount < expectedWhCount) {
            std::cerr << "Expected data for " << expectedWhCount
                      << " warehouses, but found only " << whCount << std::endl;
            std::exit(1);
        }
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for run failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

} // namespace NTpcc
