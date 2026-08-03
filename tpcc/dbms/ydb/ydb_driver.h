#pragma once

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <memory>
#include <string>

namespace NTpcc {

struct TYdbConnectionConfig {
    std::string Endpoint;
    std::string Database;
    std::string Path;
    std::string Token;
    std::string TokenEnv;
    std::string SaKeyFile;
};

std::string ReadYdbToken(const TYdbConnectionConfig& config);
NYdb::TDriverConfig BuildYdbDriverConfig(const TYdbConnectionConfig& config);

class TYdbConnection {
public:
    explicit TYdbConnection(TYdbConnectionConfig config);
    ~TYdbConnection();

    NYdb::TDriver& Driver();
    NYdb::NQuery::TQueryClient& QueryClient();
    NYdb::NTable::TTableClient& TableClient();

    const TYdbConnectionConfig& Config() const;
    std::string TablePath(const std::string& table) const;

private:
    TYdbConnectionConfig Config_;
    NYdb::TDriver Driver_;
    NYdb::NQuery::TQueryClient QueryClient_;
    NYdb::NTable::TTableClient TableClient_;
};

} // namespace NTpcc
