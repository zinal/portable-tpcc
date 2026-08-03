#include "path_checker.h"

#include "ob_connection.h"

#include <constants.h>
#include <log.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace NTpcc {
namespace {

std::unique_ptr<TObConnection> ConnectChecked(const TObConnectionConfig& cfg) {
    const std::string db = EffectiveDatabase(cfg);
    if (db.empty()) {
        throw std::runtime_error("No database specified: set connection database=... or --path");
    }

    auto conn = TObConnection::Connect(cfg, false);
    auto exists = conn->Query(
        "SELECT 1 AS ok FROM information_schema.schemata WHERE schema_name = ? LIMIT 1",
        MakeParams(db));
    if (!exists.TryNextRow()) {
        return conn;
    }
    conn->UseDatabase(db);
    return conn;
}

std::unordered_set<std::string> ListTables(TObConnection& conn, const std::string& database) {
    auto result = conn.Query(
        "SELECT table_name AS table_name FROM information_schema.tables "
        "WHERE table_schema = ? AND table_type = 'BASE TABLE'",
        MakeParams(database));

    std::unordered_set<std::string> tables;
    while (result.TryNextRow()) {
        tables.insert(result.GetString("table_name"));
    }
    return tables;
}

std::unordered_set<std::string> ListIndexes(
    TObConnection& conn,
    const std::string& database,
    const std::string& tableName)
{
    auto result = conn.Query(
        "SELECT index_name AS index_name FROM information_schema.statistics "
        "WHERE table_schema = ? AND table_name = ?",
        MakeParams(database, tableName));

    std::unordered_set<std::string> indexes;
    while (result.TryNextRow()) {
        indexes.insert(result.GetString("index_name"));
    }
    return indexes;
}

void CheckTablesExist(TObConnection& conn, const std::string& database, const char* what) {
    auto tables = ListTables(conn, database);
    for (const char* table : TPCC_TABLES) {
        if (!tables.contains(table)) {
            std::cerr << "TPC-C table '" << table << "' is missing. " << what << std::endl;
            std::exit(1);
        }
    }
}

void CheckNoTablesExist(TObConnection& conn, const std::string& database, const char* what) {
    auto tables = ListTables(conn, database);
    for (const char* table : TPCC_TABLES) {
        if (tables.contains(table)) {
            std::cerr << "TPC-C table '" << table << "' already exists. " << what << std::endl;
            std::exit(1);
        }
    }
}

void CheckIndexExists(
    TObConnection& conn,
    const std::string& database,
    const std::string& tableName,
    const std::string& expectedIndex)
{
    auto indexes = ListIndexes(conn, database, tableName);
    if (!indexes.contains(expectedIndex)) {
        std::cerr << "Table '" << tableName
                  << "' is missing expected index '" << expectedIndex
                  << "'. Run 'tpcc import' to create indexes after bulk load."
                  << std::endl;
        std::exit(1);
    }
}

int GetWarehouseCount(TObConnection& conn) {
    auto result = conn.QuerySimple("SELECT COUNT(*) AS cnt FROM warehouse");
    if (!result.TryNextRow()) {
        return 0;
    }
    return result.GetInt32("cnt");
}

} // namespace

void CheckDbForInit(const std::string& connectionString, const std::string& path) noexcept {
    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectChecked(cfg);
        CheckNoTablesExist(*conn, db, "Already inited or forgot to clean?");
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for init failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

void CheckDbForImport(const std::string& connectionString, const std::string& path) noexcept {
    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectChecked(cfg);
        CheckTablesExist(*conn, db, "Run 'tpcc init' first.");

        int whCount = GetWarehouseCount(*conn);
        if (whCount != 0) {
            LOG_W("Database already has " << whCount
                  << " warehouses; import will replace assigned ranges");
        }
    } catch (const std::exception& e) {
        std::cerr << "Pre-flight check for import failed: " << e.what() << std::endl;
        std::exit(1);
    }
}

void CheckDbForRun(
    const std::string& connectionString,
    int expectedWhCount,
    const std::string& path) noexcept
{
    try {
        auto cfg = ConfigWithPath(connectionString, path);
        const std::string db = EffectiveDatabase(cfg);
        auto conn = ConnectChecked(cfg);
        CheckTablesExist(*conn, db, "Run 'tpcc init' and 'tpcc import' first.");
        CheckIndexExists(*conn, db, TABLE_CUSTOMER, INDEX_CUSTOMER_NAME);
        CheckIndexExists(*conn, db, TABLE_OORDER, INDEX_ORDER);

        int whCount = GetWarehouseCount(*conn);
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
