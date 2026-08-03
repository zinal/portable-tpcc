#include "ydb_driver.h"

#include <log.h>

#include <cstdlib>
#include <stdexcept>

namespace NTpcc {

std::string ReadYdbToken(const TYdbConnectionConfig& config) {
    if (!config.Token.empty()) {
        return config.Token;
    }
    if (!config.TokenEnv.empty()) {
        const char* value = std::getenv(config.TokenEnv.c_str());
        if (!value) {
            throw std::runtime_error("YDB token environment variable is not set: " + config.TokenEnv);
        }
        return value;
    }
    return {};
}

NYdb::TDriverConfig BuildYdbDriverConfig(const TYdbConnectionConfig& config) {
    if (config.Endpoint.empty()) {
        throw std::runtime_error("YDB endpoint is empty");
    }
    if (config.Database.empty()) {
        throw std::runtime_error("YDB database is empty");
    }

    NYdb::TDriverConfig driverConfig;
    driverConfig.SetEndpoint(config.Endpoint);
    driverConfig.SetDatabase(config.Database);

    const std::string token = ReadYdbToken(config);
    if (!token.empty()) {
        driverConfig.SetAuthToken(token);
    }

    if (!config.SaKeyFile.empty()) {
        LOG_I("YDB service-account key file flag is accepted but this build uses token auth");
    }

    return driverConfig;
}

TYdbConnection::TYdbConnection(TYdbConnectionConfig config)
    : Config_(std::move(config))
    , Driver_(BuildYdbDriverConfig(Config_))
    , QueryClient_(Driver_)
    , TableClient_(Driver_)
{
}

TYdbConnection::~TYdbConnection() {
    Driver_.Stop(true);
}

NYdb::TDriver& TYdbConnection::Driver() {
    return Driver_;
}

NYdb::NQuery::TQueryClient& TYdbConnection::QueryClient() {
    return QueryClient_;
}

NYdb::NTable::TTableClient& TYdbConnection::TableClient() {
    return TableClient_;
}

const TYdbConnectionConfig& TYdbConnection::Config() const {
    return Config_;
}

std::string TYdbConnection::TablePath(const std::string& table) const {
    if (Config_.Path.empty()) {
        return Config_.Database + "/" + table;
    }
    if (!Config_.Path.empty() && Config_.Path.front() == '/') {
        return Config_.Path + "/" + table;
    }
    return Config_.Database + "/" + Config_.Path + "/" + table;
}

} // namespace NTpcc
