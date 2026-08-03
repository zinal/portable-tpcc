#include "clean.h"

#include "ob_connection.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>

#include <stdexcept>

namespace NTpcc {
namespace {

constexpr const char* TABLEGROUP_TPCC = "tpcc_tg";

void DropTable(TObConnection& conn, const char* tableName) {
    const std::string sql = "DROP TABLE IF EXISTS " + QuoteIdent(tableName);
    LOG_T("Dropping table " << tableName);
    try {
        conn.ExecuteSimple(sql);
        LOG_I("Table " << tableName << " dropped successfully");
    } catch (const std::exception& ex) {
        LOG_W("Failed to drop table " << tableName << ": " << ex.what());
    }
}

} // namespace

void CleanSync(const std::string& connectionString, const std::string& path) {
    auto cfg = ConfigWithPath(connectionString, path);
    const std::string db = EffectiveDatabase(cfg);
    if (db.empty()) {
        throw std::runtime_error("No database specified: set connection database=... or --path");
    }

    auto conn = TObConnection::Connect(cfg, false);
    auto exists = conn->Query(
        "SELECT 1 AS ok FROM information_schema.schemata WHERE schema_name = ? LIMIT 1",
        MakeParams(db));
    if (!exists.TryNextRow()) {
        LOG_I("Database '" << db << "' does not exist; nothing to clean");
        return;
    }

    conn->UseDatabase(db);
    LOG_I("Starting to drop TPC-C tables in database '" << db << "'");

    DropTable(*conn, TABLE_ORDER_LINE);
    DropTable(*conn, TABLE_NEW_ORDER);
    DropTable(*conn, TABLE_OORDER);
    DropTable(*conn, TABLE_HISTORY);
    DropTable(*conn, TABLE_CUSTOMER);
    DropTable(*conn, TABLE_DISTRICT);
    DropTable(*conn, TABLE_STOCK);
    DropTable(*conn, TABLE_ITEM);
    DropTable(*conn, TABLE_WAREHOUSE);

    try {
        conn->ExecuteSimple(fmt::format("DROP TABLEGROUP IF EXISTS {}", TABLEGROUP_TPCC));
        LOG_I("Table group '" << TABLEGROUP_TPCC << "' dropped");
    } catch (const std::exception& ex) {
        LOG_W("Failed to drop table group '" << TABLEGROUP_TPCC << "': " << ex.what());
    }

    if (!path.empty()) {
        try {
            conn->ExecuteSimple("USE information_schema");
            conn->ExecuteSimple("DROP DATABASE IF EXISTS " + QuoteIdent(path));
            LOG_I("Database '" << path << "' dropped");
        } catch (const std::exception& ex) {
            LOG_W("Failed to drop database '" << path << "': " << ex.what());
        }
    }

    LOG_I("All TPC-C tables dropped successfully");
}

} // namespace NTpcc
