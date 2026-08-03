#pragma once

#include "ob_params.h"
#include "ob_queries.h"
#include "query_result.h"

#include <memory>
#include <string>

namespace NTpcc {

struct TObConnectionConfig {
    std::string Host = "127.0.0.1";
    int Port = 2881;
    std::string User = "root@test";
    std::string Password;
    std::string Database = "tpcc";
    std::string Path;
};

TObConnectionConfig ParseConnectionString(const std::string& connection);
std::string EffectiveDatabase(const TObConnectionConfig& config);
std::string QuoteIdent(const std::string& ident);
std::string ObClientVersion();

struct TObConnection {
    TObConnection() = default;
    ~TObConnection();

    TObConnection(const TObConnection&) = delete;
    TObConnection& operator=(const TObConnection&) = delete;

    static std::unique_ptr<TObConnection> Connect(
        const TObConnectionConfig& config,
        bool selectDatabase = true);

    void UseDatabase(const std::string& database);
    void CreateDatabaseIfNotExists(const std::string& database);
    void BeginRepeatableRead();
    void Commit();
    void Rollback();

    QueryResult Query(EObQueryId queryId, const TObParams& params = {});
    uint64_t Execute(EObQueryId queryId, const TObParams& params = {});

    QueryResult Query(const std::string& sql, const TObParams& params = {});
    uint64_t Execute(const std::string& sql, const TObParams& params = {});

    void Reconnect(const TObConnectionConfig& config, bool selectDatabase = true);

    QueryResult QuerySimple(const std::string& sql);
    uint64_t ExecuteSimple(const std::string& sql);

    void KillQuery(const TObConnectionConfig& adminConfig);
    unsigned long ThreadId() const;
    bool Ok() const;

private:
    void EstablishConnection(const TObConnectionConfig& config, bool selectDatabase);

    struct TImpl;
    std::unique_ptr<TImpl> Impl_;
};

TObConnectionConfig ConfigWithPath(const std::string& connectionString, const std::string& path);
std::unique_ptr<TObConnection> ConnectToTargetDatabase(const TObConnectionConfig& config);
bool IsOceanBaseServer(TObConnection& conn);

} // namespace NTpcc
