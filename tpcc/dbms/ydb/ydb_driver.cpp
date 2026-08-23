#include "ydb_driver.h"

#include <log.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/discovery/discovery.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/iam/iam.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/credentials/credentials.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/types/ydb.h>

#include <password_secret.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_set>

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

    // Query Service CreateSession is executed on the node that receives the
    // RPC. Prefer all discovered nodes over the default "local DC + lowest
    // load_factor" elector, which otherwise piles a burst of sessions onto
    // one node (check --threads, worker Begin, …).
    driverConfig.SetBalancingPolicy(NYdb::TBalancingPolicy::UseAllNodes());

    return driverConfig;
}

namespace {

constexpr uint32_t kMaxQuerySessions = 8192;

NYdb::NQuery::TClientSettings MakeQueryClientSettings() {
    return NYdb::NQuery::TClientSettings()
        .SessionPoolSettings(
            NYdb::NQuery::TSessionPoolSettings()
                .MaxActiveSessions(kMaxQuerySessions)
                .MinPoolSize(0));
}

std::string PickDiscoveredNodeAddress(const NYdb::NDiscovery::TEndpointInfo& endpoint) {
    if (!endpoint.Address.empty()) {
        return endpoint.Address;
    }
    if (!endpoint.IPv4Addrs.empty()) {
        return endpoint.IPv4Addrs.front();
    }
    if (!endpoint.IPv6Addrs.empty()) {
        return endpoint.IPv6Addrs.front();
    }
    return {};
}

} // anonymous

std::string FormatYdbNodeHostPort(const std::string& address, uint32_t port) {
    if (address.empty() || port == 0) {
        return {};
    }
    if (address.find(':') != std::string::npos && address.front() != '[') {
        return "[" + address + "]:" + std::to_string(port);
    }
    return address + ":" + std::to_string(port);
}

std::vector<std::string> UniqueYdbNodeHostPorts(const std::vector<TYdbDiscoveredNode>& nodes) {
    std::vector<std::string> hostPorts;
    std::unordered_set<uint32_t> seenNodeIds;
    std::unordered_set<std::string> seenHostPorts;
    hostPorts.reserve(nodes.size());
    for (const auto& node : nodes) {
        const std::string hostPort = FormatYdbNodeHostPort(node.Address, node.Port);
        if (hostPort.empty()) {
            continue;
        }
        if (node.NodeId != 0 && !seenNodeIds.insert(node.NodeId).second) {
            continue;
        }
        if (!seenHostPorts.insert(hostPort).second) {
            continue;
        }
        hostPorts.push_back(hostPort);
    }
    return hostPorts;
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

void TYdbConnection::InitNodeQueryClients() {
    try {
        NYdb::NDiscovery::TDiscoveryClient discovery(Driver_);
        const auto result = discovery.ListEndpoints().GetValueSync();
        if (!result.IsSuccess()) {
            LOG_W("YDB ListEndpoints failed; query sessions use the shared client: "
                  << result.GetIssues().ToOneLineString());
            return;
        }

        std::vector<TYdbDiscoveredNode> nodes;
        nodes.reserve(result.GetEndpointsInfo().size());
        for (const auto& endpoint : result.GetEndpointsInfo()) {
            TYdbDiscoveredNode node;
            node.NodeId = endpoint.NodeId;
            node.Address = PickDiscoveredNodeAddress(endpoint);
            node.Port = endpoint.Port;
            nodes.push_back(std::move(node));
        }

        const std::vector<std::string> hostPorts = UniqueYdbNodeHostPorts(nodes);
        if (hostPorts.size() <= 1) {
            LOG_D("YDB discovered " << hostPorts.size()
                  << " query node(s); using the shared QueryClient");
            return;
        }

        NodeQueryClients_.reserve(hostPorts.size());
        for (const auto& hostPort : hostPorts) {
            auto settings = MakeQueryClientSettings();
            settings.DiscoveryEndpoint(hostPort);
            settings.DiscoveryMode(NYdb::EDiscoveryMode::Off);
            NodeQueryClients_.emplace_back(Driver_, settings);
        }
        LOG_I("YDB query sessions will be spread across "
              << NodeQueryClients_.size() << " nodes");
    } catch (const std::exception& ex) {
        LOG_W("YDB node discovery for session spread failed; using the shared QueryClient: "
              << ex.what());
    }
}

NYdb::NQuery::TQueryClient& TYdbConnection::QueryClient() {
    std::call_once(NodeQueryClientsOnce_, [this] { InitNodeQueryClients(); });
    if (NodeQueryClients_.empty()) {
        return QueryClient_;
    }
    const size_t i = NextNodeQueryClient_.fetch_add(1, std::memory_order_relaxed);
    return NodeQueryClients_[i % NodeQueryClients_.size()];
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
