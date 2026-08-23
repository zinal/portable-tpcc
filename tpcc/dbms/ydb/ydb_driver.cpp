#include "ydb_driver.h"

#include <log.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/iam/iam.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/resources/ydb_resources.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/ydb.h>

#include <password_secret.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace NTpcc {

namespace {

std::string ReadFileContents(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string NormalizeRelativePrefix(const TYdbConnectionConfig& config) {
    std::string prefix = config.Path;
    if (!prefix.empty() && prefix.front() == '/') {
        const std::string& database = config.Database;
        if (prefix == database) {
            prefix.clear();
        } else if (prefix.size() > database.size()
                   && prefix.compare(0, database.size(), database) == 0
                   && prefix[database.size()] == '/')
        {
            prefix = prefix.substr(database.size() + 1);
        } else {
            throw std::runtime_error(
                "YDB path must be inside the database: path=" + config.Path
                + ", database=" + database);
        }
    }
    return prefix;
}

// Strip grpc(s):// and detect TLS. Endpoint forms:
//   host:port | grpc://host:port | grpcs://host:port
void ApplyEndpoint(NYdb::TDriverConfig& driverConfig, const std::string& endpoint, bool& enableSsl) {
    if (endpoint.empty()) {
        throw std::runtime_error("YDB endpoint is empty");
    }

    static const std::string kGrpc = "grpc://";
    static const std::string kGrpcs = "grpcs://";
    std::string hostPort = endpoint;
    enableSsl = false;

    if (endpoint.rfind(kGrpcs, 0) == 0) {
        enableSsl = true;
        hostPort = endpoint.substr(kGrpcs.size());
    } else if (endpoint.rfind(kGrpc, 0) == 0) {
        hostPort = endpoint.substr(kGrpc.size());
    } else if (endpoint.find("://") != std::string::npos) {
        throw std::runtime_error(
            "YDB endpoint scheme must be grpc:// or grpcs:// (or omit the scheme)");
    }

    if (hostPort.empty()) {
        throw std::runtime_error("YDB endpoint host is empty");
    }
    driverConfig.SetEndpoint(hostPort);
}

void ApplySecureConnection(
    NYdb::TDriverConfig& driverConfig,
    bool enableSsl,
    const std::string& caFile)
{
    if (!enableSsl && caFile.empty()) {
        return;
    }
    std::string caCerts;
    if (!caFile.empty()) {
        caCerts = ReadFileContents(caFile);
        if (caCerts.empty()) {
            throw std::runtime_error("YDB CA certificate file is empty: " + caFile);
        }
    }
    driverConfig.UseSecureConnection(caCerts);
}

void ApplyCredentials(NYdb::TDriverConfig& driverConfig, const TYdbConnectionConfig& config) {
    switch (config.AuthScheme) {
        case EYdbAuthScheme::Anonymous:
            driverConfig.SetCredentialsProviderFactory(
                NYdb::CreateInsecureCredentialsProviderFactory());
            return;
        case EYdbAuthScheme::Login: {
            if (config.User.empty()) {
                throw std::runtime_error("YDB login auth requires user");
            }
            NYdb::TLoginCredentialsParams params;
            params.User = config.User;
            // PasswordFile is already resolved under run_dir when present.
            params.Password = ReadDatabasePassword(
                config.PasswordFile, config.PasswordEnv, /*runDir=*/"");
            driverConfig.SetCredentialsProviderFactory(
                NYdb::CreateLoginCredentialsProviderFactory(std::move(params)));
            return;
        }
        case EYdbAuthScheme::SaKey: {
            if (config.SaKeyFile.empty()) {
                throw std::runtime_error("YDB sa_key auth requires sa_key_file");
            }
            NYdb::TIamJwtFilename params;
            params.JwtFilename = config.SaKeyFile;
            driverConfig.SetCredentialsProviderFactory(
                NYdb::CreateIamJwtFileCredentialsProviderFactory(params));
            return;
        }
        case EYdbAuthScheme::Token: {
            const std::string token = ReadYdbToken(config);
            if (token.empty()) {
                throw std::runtime_error("YDB token auth requires token or token_env");
            }
            driverConfig.SetAuthToken(token);
            return;
        }
    }
    throw std::runtime_error("unsupported YDB auth_scheme");
}

} // anonymous

bool ParseYdbAuthScheme(const std::string& value, EYdbAuthScheme& out) {
    if (value == "anonymous") {
        out = EYdbAuthScheme::Anonymous;
        return true;
    }
    if (value == "login") {
        out = EYdbAuthScheme::Login;
        return true;
    }
    if (value == "sa_key") {
        out = EYdbAuthScheme::SaKey;
        return true;
    }
    if (value == "token") {
        out = EYdbAuthScheme::Token;
        return true;
    }
    return false;
}

const char* YdbAuthSchemeToString(EYdbAuthScheme scheme) {
    switch (scheme) {
        case EYdbAuthScheme::Anonymous:
            return "anonymous";
        case EYdbAuthScheme::Login:
            return "login";
        case EYdbAuthScheme::SaKey:
            return "sa_key";
        case EYdbAuthScheme::Token:
            return "token";
    }
    return "unknown";
}

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
    if (config.Database.empty()) {
        throw std::runtime_error("YDB database is empty");
    }

    NYdb::TDriverConfig driverConfig;
    bool enableSsl = false;
    ApplyEndpoint(driverConfig, config.Endpoint, enableSsl);
    driverConfig.SetDatabase(config.Database);
    ApplySecureConnection(driverConfig, enableSsl, config.CaFile);
    ApplyCredentials(driverConfig, config);

    const bool ssl = enableSsl || !config.CaFile.empty();
    if (config.CaFile.empty()) {
        LOG_D("YDB auth_scheme=" << YdbAuthSchemeToString(config.AuthScheme)
              << " ssl=" << (ssl ? "on" : "off"));
    } else {
        LOG_D("YDB auth_scheme=" << YdbAuthSchemeToString(config.AuthScheme)
              << " ssl=on ca_file=" << config.CaFile);
    }

    // Same policy as `ydb workload`: consider every discovered node, not only
    // the preferred location. Session placement itself is still done by the
    // SDK elector + server session-balancer (see MakeYdbCreateSessionSettings).
    driverConfig.SetBalancingPolicy(NYdb::TBalancingPolicy::UseAllNodes());

    return driverConfig;
}

namespace {

// Default QueryClient pool is 50 active / 10 idle — too small for check
// --threads and for a worker with many terminals. The pool does not
// pre-create MaxActiveSessions.
constexpr uint32_t kMaxQuerySessions = 8192;

NYdb::NQuery::TClientSettings MakeQueryClientSettings() {
    return NYdb::NQuery::TClientSettings()
        .SessionPoolSettings(
            NYdb::NQuery::TSessionPoolSettings()
                .MaxActiveSessions(kMaxQuerySessions)
                .MinPoolSize(0));
}

} // anonymous

NYdb::NQuery::TCreateSessionSettings MakeYdbCreateSessionSettings() {
    NYdb::NQuery::TCreateSessionSettings settings;
    settings.Header({
        {NYdb::YDB_CLIENT_CAPABILITIES, NYdb::YDB_CLIENT_CAPABILITY_SESSION_BALANCER},
    });
    return settings;
}

TYdbConnection::TYdbConnection(TYdbConnectionConfig config)
    : Config_(std::move(config))
    , Driver_(BuildYdbDriverConfig(Config_))
    , QueryClient_(Driver_, MakeQueryClientSettings())
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

std::string TYdbConnection::RelativeTablePath(const std::string& table) const {
    const std::string prefix = NormalizeRelativePrefix(Config_);
    if (prefix.empty()) {
        return table;
    }
    return prefix + "/" + table;
}

std::string TYdbConnection::TablePath(const std::string& table) const {
    // BulkUpsert / scheme APIs require the absolute path including the database.
    return Config_.Database + "/" + RelativeTablePath(table);
}

std::string TYdbConnection::AbsolutePathPrefix() const {
    // PRAGMA TablePathPrefix requires the full path including the database name.
    const std::string prefix = NormalizeRelativePrefix(Config_);
    if (prefix.empty()) {
        return Config_.Database;
    }
    return Config_.Database + "/" + prefix;
}

} // namespace NTpcc
