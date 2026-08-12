#include "path_checker.h"

#include <constants.h>
#include <log.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/scheme/scheme.h>

#include <cstdlib>

namespace NTpcc {

namespace {

void CheckTables(const TYdbConnectionConfig& connectionConfig) {
    TYdbConnection connection(connectionConfig);
    NYdb::NScheme::TSchemeClient scheme(connection.Driver());
    for (const auto* table : TPCC_TABLES) {
        auto status = scheme.DescribePath(connection.TablePath(table)).GetValueSync();
        if (!status.IsSuccess()) {
            LOG_E("YDB table is not available: " << connection.TablePath(table)
                  << ": " << status.GetIssues().ToOneLineString());
            std::exit(1);
        }
    }
}

} // anonymous

void CheckDbForInit(const TYdbConnectionConfig& /*connectionConfig*/) noexcept {
}

void CheckDbForImport(const TYdbConnectionConfig& connectionConfig) noexcept {
    CheckTables(connectionConfig);
}

void CheckDbForIndexes(const TYdbConnectionConfig& connectionConfig) noexcept {
    CheckTables(connectionConfig);
}

void CheckDbForRun(const TYdbConnectionConfig& connectionConfig, int /*expectedWhCount*/) noexcept {
    CheckTables(connectionConfig);
}

} // namespace NTpcc
