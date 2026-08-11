#include "ob_connection.h"

#include "ob_errors.h"
#include "ob_prepared_statement.h"

#include <log.h>

#include <mysql.h>

#include <cctype>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace NTpcc {
namespace {

[[noreturn]] void ThrowMysqlError(MYSQL* mysql, const char* what) {
    const int code = mysql ? static_cast<int>(mysql_errno(mysql)) : 0;
    const char* msg = mysql ? mysql_error(mysql) : "null mysql handle";
    throw TObDbError(code, std::string(what) + ": [" + std::to_string(code) + "] " + msg);
}

std::string Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

void SetKv(TObConnectionConfig& cfg, const std::string& key, const std::string& value) {
    if (key == "host" || key == "hostname") {
        cfg.Host = value;
    } else if (key == "port") {
        cfg.Port = std::stoi(value);
    } else if (key == "user" || key == "uid") {
        cfg.User = value;
    } else if (key == "password" || key == "pwd" || key == "passwd") {
        cfg.Password = value;
    } else if (key == "database" || key == "db" || key == "dbname") {
        cfg.Database = value;
    } else if (key == "path") {
        cfg.Path = value;
    }
}

QueryResult MaterializeResult(MYSQL_RES* res) {
    if (!res) {
        return QueryResult{};
    }

    const unsigned numFields = mysql_num_fields(res);
    MYSQL_FIELD* fields = mysql_fetch_fields(res);

    std::vector<std::string> columns;
    columns.reserve(numFields);
    for (unsigned i = 0; i < numFields; ++i) {
        columns.emplace_back(fields[i].name ? fields[i].name : "");
    }

    std::vector<std::vector<std::optional<std::string>>> rows;
    rows.reserve(mysql_num_rows(res));
    while (MYSQL_ROW row = mysql_fetch_row(res)) {
        unsigned long* lengths = mysql_fetch_lengths(res);
        std::vector<std::optional<std::string>> values;
        values.reserve(numFields);
        for (unsigned i = 0; i < numFields; ++i) {
            if (row[i] == nullptr) {
                values.emplace_back(std::nullopt);
            } else {
                values.emplace_back(std::string(row[i], lengths[i]));
            }
        }
        rows.push_back(std::move(values));
    }

    mysql_free_result(res);
    return QueryResult(std::move(columns), std::move(rows));
}

void SetSessionRepeatableRead(MYSQL* mysql) {
    if (mysql_query(mysql, "SET SESSION TRANSACTION ISOLATION LEVEL REPEATABLE READ") != 0) {
        ThrowMysqlError(mysql, "SET SESSION TRANSACTION ISOLATION LEVEL failed");
    }
}

} // namespace

struct TObConnection::TImpl {
    MYSQL* Mysql = nullptr;
    TObConnectionConfig Config;
    bool SelectDatabase = true;
    std::unique_ptr<TObStatementCache> StmtCache;

    void InitStatementCache() {
        StmtCache = std::make_unique<TObStatementCache>(static_cast<void*>(Mysql));
    }

    void ClearStatementCache() {
        if (StmtCache) {
            StmtCache->Clear();
            StmtCache.reset();
        }
    }
};

TObConnectionConfig ParseConnectionString(const std::string& connection) {
    TObConnectionConfig cfg;
    if (connection.empty()) {
        return cfg;
    }

    const char sep = (connection.find(';') != std::string::npos) ? ';' : ' ';
    std::string token;
    std::istringstream iss(connection);
    while (std::getline(iss, token, sep)) {
        token = Trim(token);
        if (token.empty()) {
            continue;
        }
        const auto eq = token.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        SetKv(cfg, Trim(token.substr(0, eq)), Trim(token.substr(eq + 1)));
    }
    return cfg;
}

std::string EffectiveDatabase(const TObConnectionConfig& config) {
    if (!config.Path.empty()) {
        return config.Path;
    }
    return config.Database;
}

std::string QuoteIdent(const std::string& ident) {
    if (ident.empty()) {
        throw std::invalid_argument("empty SQL identifier");
    }
    for (unsigned char ch : ident) {
        if (!(std::isalnum(ch) || ch == '_')) {
            throw std::invalid_argument(
                "invalid SQL identifier '" + ident + "' (use [A-Za-z0-9_]+)");
        }
    }
    return "`" + ident + "`";
}

std::string ObClientVersion() {
    return mysql_get_client_info();
}

void TObConnection::EstablishConnection(const TObConnectionConfig& config, bool selectDatabase) {
    Impl_->Mysql = mysql_init(nullptr);
    if (!Impl_->Mysql) {
        throw std::runtime_error("mysql_init failed");
    }

#ifdef MYSQL_OPT_SSL_ENFORCE
    const int sslEnforce = 0;
    mysql_options(Impl_->Mysql, MYSQL_OPT_SSL_ENFORCE, &sslEnforce);
#endif

    const std::string selectedDb = selectDatabase ? EffectiveDatabase(config) : std::string{};
    const char* initialDb = selectedDb.empty() ? nullptr : selectedDb.c_str();

    if (!mysql_real_connect(
            Impl_->Mysql,
            config.Host.c_str(),
            config.User.c_str(),
            config.Password.c_str(),
            initialDb,
            static_cast<unsigned int>(config.Port),
            nullptr,
            CLIENT_MULTI_STATEMENTS | CLIENT_FOUND_ROWS))
    {
        ThrowMysqlError(Impl_->Mysql, "mysql_real_connect failed");
    }

    SetSessionRepeatableRead(Impl_->Mysql);
    Impl_->Config = config;
    Impl_->SelectDatabase = selectDatabase;
    Impl_->InitStatementCache();
}

std::unique_ptr<TObConnection> TObConnection::Connect(
    const TObConnectionConfig& config,
    bool selectDatabase)
{
    auto conn = std::unique_ptr<TObConnection>(new TObConnection());
    conn->Impl_ = std::make_unique<TImpl>();
    conn->EstablishConnection(config, selectDatabase);
    return conn;
}

TObConnection::~TObConnection() {
    if (Impl_) {
        Impl_->ClearStatementCache();
        if (Impl_->Mysql) {
            mysql_close(Impl_->Mysql);
            Impl_->Mysql = nullptr;
        }
    }
}

void TObConnection::Reconnect(const TObConnectionConfig& config, bool selectDatabase) {
    Impl_->ClearStatementCache();
    if (Impl_->Mysql) {
        mysql_close(Impl_->Mysql);
        Impl_->Mysql = nullptr;
    }
    EstablishConnection(config, selectDatabase);
}

void TObConnection::UseDatabase(const std::string& database) {
    if (mysql_select_db(Impl_->Mysql, database.c_str()) != 0) {
        ThrowMysqlError(Impl_->Mysql, "USE database failed");
    }
}

void TObConnection::CreateDatabaseIfNotExists(const std::string& database) {
    const std::string sql = "CREATE DATABASE IF NOT EXISTS " + QuoteIdent(database);
    if (mysql_query(Impl_->Mysql, sql.c_str()) != 0) {
        ThrowMysqlError(Impl_->Mysql, "CREATE DATABASE failed");
    }
}

void TObConnection::ConfigureBulkLoadSession() {
    // Match tpcc-oceanbase-cpp ImportQueryTimeoutMicros (600s). Default is 10s.
    constexpr const char* kSql = "SET SESSION ob_query_timeout = 600000000";
    if (mysql_query(Impl_->Mysql, kSql) != 0) {
        // MariaDB stand-in and other MySQL-compat servers may not have this variable.
        LOG_W("Could not set ob_query_timeout ("
              << (Impl_->Mysql ? mysql_error(Impl_->Mysql) : "null mysql handle")
              << "); continuing with server default");
    }
}

void TObConnection::BeginRepeatableRead() {
    if (mysql_query(Impl_->Mysql, "START TRANSACTION") != 0) {
        ThrowMysqlError(Impl_->Mysql, "START TRANSACTION failed");
    }
}

void TObConnection::Commit() {
    if (mysql_query(Impl_->Mysql, "COMMIT") != 0) {
        ThrowMysqlError(Impl_->Mysql, "COMMIT failed");
    }
}

void TObConnection::Rollback() {
    if (!Impl_ || !Impl_->Mysql) {
        return;
    }
    if (mysql_query(Impl_->Mysql, "ROLLBACK") != 0) {
    }
}

QueryResult TObConnection::Query(EObQueryId queryId, const TObParams& params) {
    return Impl_->StmtCache->Query(queryId, params);
}

uint64_t TObConnection::Execute(EObQueryId queryId, const TObParams& params) {
    return Impl_->StmtCache->Execute(queryId, params);
}

QueryResult TObConnection::Query(const std::string& sql, const TObParams& params) {
    if (!params.Empty()) {
        return Impl_->StmtCache->QueryText(sql, params);
    }

    if (mysql_real_query(Impl_->Mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        ThrowMysqlError(Impl_->Mysql, "Query failed");
    }
    MYSQL_RES* res = mysql_store_result(Impl_->Mysql);
    if (!res && mysql_field_count(Impl_->Mysql) != 0) {
        ThrowMysqlError(Impl_->Mysql, "mysql_store_result failed");
    }
    return MaterializeResult(res);
}

uint64_t TObConnection::Execute(const std::string& sql, const TObParams& params) {
    if (!params.Empty()) {
        return Impl_->StmtCache->ExecuteText(sql, params);
    }

    if (mysql_real_query(Impl_->Mysql, sql.data(), static_cast<unsigned long>(sql.size())) != 0) {
        ThrowMysqlError(Impl_->Mysql, "Execute failed");
    }
    do {
        if (MYSQL_RES* res = mysql_store_result(Impl_->Mysql)) {
            mysql_free_result(res);
        }
    } while (mysql_next_result(Impl_->Mysql) == 0);

    return static_cast<uint64_t>(mysql_affected_rows(Impl_->Mysql));
}

QueryResult TObConnection::QuerySimple(const std::string& sql) {
    return Query(sql, TObParams{});
}

uint64_t TObConnection::ExecuteSimple(const std::string& sql) {
    return Execute(sql, TObParams{});
}

void TObConnection::KillQuery(const TObConnectionConfig& adminConfig) {
    if (!Impl_ || !Impl_->Mysql) {
        return;
    }
    const unsigned long tid = mysql_thread_id(Impl_->Mysql);
    try {
        auto admin = Connect(adminConfig);
        admin->ExecuteSimple("KILL QUERY " + std::to_string(tid));
    } catch (...) {
    }
}

unsigned long TObConnection::ThreadId() const {
    return Impl_ && Impl_->Mysql ? mysql_thread_id(Impl_->Mysql) : 0;
}

bool TObConnection::Ok() const {
    return Impl_ && Impl_->Mysql;
}

TObConnectionConfig ConfigWithPath(const std::string& connectionString, const std::string& path) {
    auto cfg = ParseConnectionString(connectionString);
    if (!path.empty()) {
        cfg.Path = path;
    }
    return cfg;
}

std::unique_ptr<TObConnection> ConnectToTargetDatabase(const TObConnectionConfig& config) {
    const std::string db = EffectiveDatabase(config);
    if (db.empty()) {
        throw std::runtime_error("No database specified: set connection database=... or --path");
    }

    auto conn = TObConnection::Connect(config, false);
    auto exists = conn->Query(
        "SELECT 1 AS ok FROM information_schema.schemata WHERE schema_name = ? LIMIT 1",
        MakeParams(db));
    if (!exists.TryNextRow()) {
        try {
            conn->CreateDatabaseIfNotExists(db);
        } catch (const std::exception& ex) {
            throw std::runtime_error(
                "Database '" + db + "' does not exist and CREATE DATABASE failed: " + ex.what());
        }
    }
    conn->UseDatabase(db);
    return conn;
}

bool IsOceanBaseServer(TObConnection& conn) {
    auto result = conn.QuerySimple("SELECT VERSION() AS v");
    if (!result.TryNextRow()) {
        return false;
    }
    const std::string version = result.GetString("v");
    return version.find("OceanBase") != std::string::npos
        || version.find("oceanbase") != std::string::npos;
}

} // namespace NTpcc
