#include "ydb_admin_adapter.h"

#include "clean.h"
#include "init.h"

#include <log.h>

namespace NTpcc {

TYdbAdminAdapter::TYdbAdminAdapter(TYdbConnectionConfig connectionConfig, int warehouseCount)
    : ConnectionConfig_(std::move(connectionConfig))
    , WarehouseCount_(warehouseCount)
{}

void TYdbAdminAdapter::EnsureSchema() {
    InitSync(ConnectionConfig_, WarehouseCount_);
}

void TYdbAdminAdapter::EnsureIndexes() {
    CreateIndexes(ConnectionConfig_);
}

void TYdbAdminAdapter::EnsureStatistics() {
    LOG_I("YDB statistics collection is managed by the database");
}

void TYdbAdminAdapter::Clean() {
    CleanSync(ConnectionConfig_);
}

TAdminDescribe TYdbAdminAdapter::Describe() {
    TAdminDescribe d;
    d.AdapterName = "ydb";
    d.ClientVersion = "ydb-cpp-sdk";
    d.ServerVersion = ConnectionConfig_.Endpoint;
    return d;
}

} // namespace NTpcc
