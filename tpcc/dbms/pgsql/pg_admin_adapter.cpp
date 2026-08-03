#include "pg_admin_adapter.h"

#include "clean.h"
#include "init.h"

#include <constants.h>
#include <log.h>

#include <pqxx/pqxx>
#include <pqxx/version.hxx>
#include <fmt/format.h>

namespace NTpcc {

namespace {

void SetSearchPath(pqxx::connection& conn, const std::string& path) {
    if (path.empty()) {
        return;
    }
    pqxx::nontransaction ntx(conn);
    ntx.exec(fmt::format("SET search_path TO {}", conn.quote_name(path)));
}

} // anonymous

TPgAdminAdapter::TPgAdminAdapter(
    std::string connectionString,
    std::string path,
    TPgPartitionConfig partitionConfig)
    : ConnectionString_(std::move(connectionString))
    , Path_(std::move(path))
    , PartitionConfig_(std::move(partitionConfig))
{
}

void TPgAdminAdapter::EnsureSchema() {
    InitSync(ConnectionString_, Path_, PartitionConfig_);
}

void TPgAdminAdapter::EnsureIndexes() {
    CreateIndexes(ConnectionString_, Path_);
}

void TPgAdminAdapter::EnsureStatistics() {
    LOG_I("Running ANALYZE on TPC-C tables...");
    pqxx::connection conn(ConnectionString_);
    SetSearchPath(conn, Path_);
    pqxx::nontransaction ntx(conn);
    for (const auto* table : TPCC_TABLES) {
        ntx.exec(fmt::format("ANALYZE {}", table));
    }
}

void TPgAdminAdapter::Clean() {
    CleanSync(ConnectionString_, Path_);
}

TAdminDescribe TPgAdminAdapter::Describe() {
    TAdminDescribe d;
    d.AdapterName = "pgsql";
    d.ClientVersion = PQXX_VERSION;
    pqxx::connection conn(ConnectionString_);
    pqxx::nontransaction ntx(conn);
    auto r = ntx.exec("SHOW server_version").front();
    d.ServerVersion = r[0].as<std::string>();
    return d;
}

} // namespace NTpcc
