#include "ob_admin_adapter.h"

#include "clean.h"
#include "init.h"
#include "ob_connection.h"

namespace NTpcc {

TObAdminAdapter::TObAdminAdapter(
    std::string connectionString,
    std::string path,
    TObSchemaOptions schemaOptions)
    : ConnectionString_(std::move(connectionString))
    , Path_(std::move(path))
    , SchemaOptions_(std::move(schemaOptions))
{}

void TObAdminAdapter::EnsureSchema() {
    InitSync(ConnectionString_, Path_, SchemaOptions_);
}

void TObAdminAdapter::EnsureIndexes() {
    CreateIndexes(ConnectionString_, Path_, false, SchemaOptions_.IndexParallel);
}

void TObAdminAdapter::EnsureStatistics() {
    AnalyzeTables(ConnectionString_, Path_);
}

void TObAdminAdapter::Clean() {
    CleanSync(ConnectionString_, Path_);
}

TAdminDescribe TObAdminAdapter::Describe() {
    TAdminDescribe d;
    d.AdapterName = "oceanbase";
    d.ClientVersion = ObClientVersion();
    auto conn = ConnectToTargetDatabase(ConfigWithPath(ConnectionString_, Path_));
    auto r = conn->QuerySimple("SELECT VERSION() AS v");
    if (r.TryNextRow()) {
        d.ServerVersion = r.GetString("v");
    }
    return d;
}

} // namespace NTpcc
