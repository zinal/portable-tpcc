#include "clean.h"

#include <constants.h>
#include <log.h>

#include <fmt/format.h>

#include <stdexcept>

namespace NTpcc {

void CleanSync(const TYdbConnectionConfig& connectionConfig) {
    LOG_I("Dropping YDB TPC-C tables...");
    TYdbConnection connection(connectionConfig);
    auto& client = connection.QueryClient();
    const std::string pragma = fmt::format("PRAGMA TablePathPrefix(\"{}\");\n", connectionConfig.Path);

    for (auto it = TPCC_TABLES.rbegin(); it != TPCC_TABLES.rend(); ++it) {
        const std::string sql = pragma + fmt::format("DROP TABLE `{}`;", *it);
        auto status = client.RetryQuerySync([&](NYdb::NQuery::TSession session) {
            return session.ExecuteQuery(sql, NYdb::NQuery::TTxControl::NoTx()).GetValueSync();
        });
        if (!status.IsSuccess() && status.GetStatus() != NYdb::EStatus::SCHEME_ERROR &&
            status.GetStatus() != NYdb::EStatus::NOT_FOUND)
        {
            throw std::runtime_error("drop table failed: " + status.GetIssues().ToOneLineString());
        }
    }
    LOG_I("YDB TPC-C tables dropped");
}

} // namespace NTpcc
