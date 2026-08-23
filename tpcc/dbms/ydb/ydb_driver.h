#pragma once

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/driver/driver.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/table/table.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

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

// host:port for a discovered node. IPv6 addresses are wrapped in [].
std::string FormatYdbNodeHostPort(const std::string& address, uint32_t port);

struct TYdbDiscoveredNode {
    uint32_t NodeId = 0;
    std::string Address;
    uint32_t Port = 0;
};

// First occurrence of each NodeId (or host:port when NodeId is 0).
std::vector<std::string> UniqueYdbNodeHostPorts(const std::vector<TYdbDiscoveredNode>& nodes);

class TYdbConnection {
public:
    explicit TYdbConnection(TYdbConnectionConfig config);
    ~TYdbConnection();

    NYdb::TDriver& Driver();
    // Round-robins across per-node QueryClients when discovery found 2+
    // nodes. Query Service CreateSession is node-local, so a single shared
    // client would pin every session to the current least-loaded node.
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
    void InitNodeQueryClients();

    TYdbConnectionConfig Config_;
    NYdb::TDriver Driver_;
    NYdb::NQuery::TQueryClient QueryClient_;
    NYdb::NTable::TTableClient TableClient_;
    std::once_flag NodeQueryClientsOnce_;
    std::vector<NYdb::NQuery::TQueryClient> NodeQueryClients_;
    std::atomic<size_t> NextNodeQueryClient_{0};
};

} // namespace NTpcc
