#pragma once

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <memory>
#include <string>

namespace NTpcc {

// YDB authentication scheme selected via profile/run-config or CLI.
// "token" is a standalone/legacy mode (token / token_env); not emitted by mind-tpcc.
enum class EYdbAuthScheme {
    Anonymous,
    Login,
    SaKey,
    Token,
};

struct TYdbConnectionConfig {
    std::string Endpoint;
    std::string Database;
    std::string Path;
    EYdbAuthScheme AuthScheme = EYdbAuthScheme::Anonymous;
    std::string User;
    std::string PasswordEnv;
    std::string PasswordFile;
    std::string Token;
    std::string TokenEnv;
    std::string SaKeyFile;
    std::string CaFile;
};

bool ParseYdbAuthScheme(const std::string& value, EYdbAuthScheme& out);
const char* YdbAuthSchemeToString(EYdbAuthScheme scheme);

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
    // Absolute path including database (required by BulkUpsert / scheme APIs).
    std::string TablePath(const std::string& table) const;
    // Path relative to the connected database for YQL without TablePathPrefix.
    std::string RelativeTablePath(const std::string& table) const;
    // Absolute directory path including database for PRAGMA TablePathPrefix.
    std::string AbsolutePathPrefix() const;

private:
    TYdbConnectionConfig Config_;
    NYdb::TDriver Driver_;
    NYdb::NQuery::TQueryClient QueryClient_;
    NYdb::NTable::TTableClient TableClient_;
};

} // namespace NTpcc
