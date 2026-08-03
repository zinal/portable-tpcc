#include "run_config.h"

#include <think_time.h>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <vector>

namespace NTpcc {

namespace {

using Json = nlohmann::json;

std::vector<TWarehouseRange> ParseWarehouseRanges(const Json& arr) {
    std::vector<TWarehouseRange> out;
    if (!arr.is_array()) {
        throw std::runtime_error("warehouse_ranges must be an array");
    }
    for (const auto& item : arr) {
        if (!item.is_array() || item.size() != 2) {
            throw std::runtime_error("warehouse range must be [start, end)");
        }
        TWarehouseRange r;
        r.Start = item[0].get<int>();
        r.End = item[1].get<int>();
        if (r.End <= r.Start) {
            throw std::runtime_error("invalid warehouse range bounds");
        }
        out.push_back(r);
    }
    return out;
}

int64_t ReadInt64(const Json& obj, const char* key, int64_t def, const std::string& path) {
    if (!obj.contains(key)) {
        return def;
    }
    const auto& value = obj[key];
    if (!value.is_number_integer()) {
        throw std::runtime_error(path + " must be an integer");
    }
    if (value.is_number_unsigned() &&
        value.get<uint64_t>() > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    {
        throw std::runtime_error(path + " is too large");
    }
    return value.get<int64_t>();
}

size_t ReadSizeTNonNegative(const Json& obj, const char* key, size_t def, const std::string& path) {
    const int64_t value = ReadInt64(obj, key, static_cast<int64_t>(def), path);
    if (value < 0) {
        throw std::runtime_error(path + " must not be negative");
    }
    return static_cast<size_t>(value);
}

int ReadIntNonNegative(const Json& obj, const char* key, int def, const std::string& path) {
    const int64_t value = ReadInt64(obj, key, def, path);
    if (value < 0) {
        throw std::runtime_error(path + " must not be negative");
    }
    if (value > std::numeric_limits<int>::max()) {
        throw std::runtime_error(path + " is too large");
    }
    return static_cast<int>(value);
}

std::string ReadFile(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open file: " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string HexEncode(const unsigned char* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = kHex[data[i] & 0xf];
    }
    return out;
}

std::string ParentDir(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    if (pos == std::string::npos) {
        return ".";
    }
    if (pos == 0) {
        return "/";
    }
    return path.substr(0, pos);
}

void ValidateRanges(
    const std::vector<TWarehouseRange>& ranges,
    int scaleWarehouses,
    const std::string& path)
{
    if (ranges.empty()) {
        throw std::runtime_error(path + " must not be empty");
    }
    for (const auto& range : ranges) {
        if (range.Start <= 0 || range.End <= range.Start) {
            throw std::runtime_error(path + " contains invalid warehouse range");
        }
        if (scaleWarehouses > 0 && range.End > scaleWarehouses + 1) {
            throw std::runtime_error(path + " exceeds scale.warehouses");
        }
    }
}

void ValidateUniqueInstance(
    std::unordered_set<std::string>& instances,
    const std::string& instance,
    const std::string& path)
{
    if (!instances.insert(instance).second) {
        throw std::runtime_error(path + " has duplicate instance: " + instance);
    }
}

void MarkWarehouseCoverage(
    std::vector<int>& ownership,
    const std::vector<TWarehouseRange>& ranges,
    const std::string& path,
    int ownerIndex)
{
    for (const auto& range : ranges) {
        for (int warehouse = range.Start; warehouse < range.End; ++warehouse) {
            if (ownership[warehouse] != 0) {
                throw std::runtime_error(
                    path + " overlaps warehouse " + std::to_string(warehouse));
            }
            ownership[warehouse] = ownerIndex;
        }
    }
}

void ValidateFullCoverage(
    const std::vector<int>& ownership,
    int scaleWarehouses,
    const std::string& path)
{
    for (int warehouse = 1; warehouse <= scaleWarehouses; ++warehouse) {
        if (ownership[warehouse] == 0) {
            throw std::runtime_error(
                path + " does not cover warehouse " + std::to_string(warehouse));
        }
    }
}

void ValidateNonNegative(int64_t value, const std::string& path) {
    if (value < 0) {
        throw std::runtime_error(path + " must not be negative");
    }
}

std::string ResolveUnderRunDir(const std::string& path, const std::string& runDir) {
    if (path.empty() || runDir.empty()) {
        return path;
    }
    if (path.front() == '/' || (path.size() >= 2 && path[1] == ':')) {
        return path;
    }
    return runDir + "/" + path;
}

EYdbAuthScheme ResolveAuthScheme(const TRunConfigDocument& doc) {
    if (!doc.AuthScheme.empty()) {
        EYdbAuthScheme scheme;
        if (!ParseYdbAuthScheme(doc.AuthScheme, scheme)) {
            throw std::runtime_error(
                "database.auth_scheme must be \"anonymous\", \"login\", or \"sa_key\"");
        }
        if (scheme == EYdbAuthScheme::Token) {
            throw std::runtime_error(
                "database.auth_scheme \"token\" is not supported in orchestrated mode");
        }
        return scheme;
    }
    if (!doc.SaKeyFile.empty()) {
        return EYdbAuthScheme::SaKey;
    }
    if (!doc.PasswordEnv.empty() || !doc.User.empty()) {
        return EYdbAuthScheme::Login;
    }
    if (!doc.TokenEnv.empty()) {
        return EYdbAuthScheme::Token;
    }
    return EYdbAuthScheme::Anonymous;
}

void ValidateYdbAuth(const TRunConfigDocument& doc) {
    const EYdbAuthScheme scheme = ResolveAuthScheme(doc);
    switch (scheme) {
        case EYdbAuthScheme::Anonymous:
            if (!doc.User.empty() || !doc.PasswordEnv.empty() || !doc.SaKeyFile.empty() ||
                !doc.TokenEnv.empty())
            {
                throw std::runtime_error(
                    "database.auth_scheme=anonymous does not accept user, password_env, "
                    "sa_key_file, or token_env");
            }
            break;
        case EYdbAuthScheme::Login:
            if (doc.User.empty()) {
                throw std::runtime_error("database.user is required for auth_scheme=login");
            }
            if (doc.PasswordEnv.empty()) {
                throw std::runtime_error(
                    "database.password_env is required for auth_scheme=login");
            }
            if (!doc.SaKeyFile.empty() || !doc.TokenEnv.empty()) {
                throw std::runtime_error(
                    "database.auth_scheme=login does not accept sa_key_file or token_env");
            }
            break;
        case EYdbAuthScheme::SaKey:
            if (doc.SaKeyFile.empty()) {
                throw std::runtime_error(
                    "database.sa_key_file is required for auth_scheme=sa_key");
            }
            if (!doc.User.empty() || !doc.PasswordEnv.empty() || !doc.TokenEnv.empty()) {
                throw std::runtime_error(
                    "database.auth_scheme=sa_key does not accept user, password_env, "
                    "or token_env");
            }
            break;
        case EYdbAuthScheme::Token:
            if (doc.TokenEnv.empty()) {
                throw std::runtime_error(
                    "database.token_env is required for auth_scheme=token");
            }
            if (!doc.User.empty() || !doc.PasswordEnv.empty() || !doc.SaKeyFile.empty()) {
                throw std::runtime_error(
                    "database.auth_scheme=token does not accept user, password_env, "
                    "or sa_key_file");
            }
            break;
    }
}

void ValidateRunConfigDocument(const TRunConfigDocument& doc, const Json& root) {
    if (doc.RunId.empty()) {
        throw std::runtime_error("run-config missing run_id");
    }
    if (doc.Dbms != "ydb") {
        throw std::runtime_error("tpcc-ydb run-config requires database.dbms=ydb");
    }
    if (doc.ScaleWarehouses <= 0) {
        throw std::runtime_error("scale.warehouses must be greater than zero");
    }
    if (doc.BatchRows < 0) {
        throw std::runtime_error("data.batch_rows must not be negative");
    }
    if (doc.WorkerAssignments.empty()) {
        throw std::runtime_error("worker_assignment must not be empty");
    }
    std::unordered_set<std::string> workerInstances;
    std::vector<int> workerOwnership(doc.ScaleWarehouses + 1, 0);
    int workerIndex = 1;
    for (const auto& worker : doc.WorkerAssignments) {
        ValidateUniqueInstance(workerInstances, worker.Instance, "worker_assignment");
        ValidateRanges(worker.WarehouseRanges, doc.ScaleWarehouses,
                       "worker_assignment warehouse_ranges");
        MarkWarehouseCoverage(
            workerOwnership, worker.WarehouseRanges, "worker_assignment", workerIndex++);
        if (worker.MaxInflight == 0) {
            throw std::runtime_error("worker_assignment.max_inflight must be greater than zero");
        }
    }
    ValidateFullCoverage(workerOwnership, doc.ScaleWarehouses, "worker_assignment");

    std::unordered_set<std::string> loaderInstances;
    std::vector<int> loaderOwnership(doc.ScaleWarehouses + 1, 0);
    int loaderIndex = 1;
    for (const auto& loader : doc.LoadAssignments) {
        ValidateUniqueInstance(loaderInstances, loader.Instance, "load_assignment");
        ValidateRanges(loader.WarehouseRanges, doc.ScaleWarehouses,
                       "load_assignment warehouse_ranges");
        MarkWarehouseCoverage(
            loaderOwnership, loader.WarehouseRanges, "load_assignment", loaderIndex++);
    }
    if (!doc.LoadAssignments.empty()) {
        ValidateFullCoverage(loaderOwnership, doc.ScaleWarehouses, "load_assignment");
    }

    ValidateNonNegative(doc.PhasePolicy.StartLeadMs, "phases.start_lead_ms");
    ValidateNonNegative(doc.PhasePolicy.RampUpMs, "phases.ramp_up_ms");
    ValidateNonNegative(doc.PhasePolicy.MeasurementMs, "phases.measurement_ms");
    ValidateNonNegative(doc.PhasePolicy.TransactionDrainMs, "phases.transaction_drain_ms");
    ValidateNonNegative(doc.PhasePolicy.StopGraceMs, "phases.stop_grace_ms");
    ValidateNonNegative(doc.PhasePolicy.MaxClockSkewMs, "phases.max_clock_skew_ms");
    if (root.contains("phases") && root["phases"].is_object()) {
        const auto& phases = root["phases"];
        if (phases.contains("async_work_drain_ms")) {
            ValidateNonNegative(
                ReadInt64(phases, "async_work_drain_ms", 0, "phases.async_work_drain_ms"),
                "phases.async_work_drain_ms");
        }
    }
    if (doc.PhasePolicy.MeasurementMs <= 0) {
        throw std::runtime_error("phases.measurement_ms must be greater than zero");
    }

    if (doc.Workload.TerminalsPerWarehouse == 0) {
        throw std::runtime_error("workload.terminals_per_warehouse must be greater than zero");
    }
    double totalWeight = 0.0;
    for (size_t i = 0; i < doc.Workload.PerTx.size(); ++i) {
        const auto& tx = doc.Workload.PerTx[i];
        if (tx.Weight < 0.0) {
            throw std::runtime_error("workload.transaction_mix weights must not be negative");
        }
        ValidateNonNegative(tx.KeyingTimeMs, "workload.keying_time_ms");
        ValidateNonNegative(tx.ThinkTimeMs, "workload.think_time_ms");
        totalWeight += tx.Weight;
    }
    if (totalWeight <= 0.0) {
        throw std::runtime_error("workload.transaction_mix must have positive total weight");
    }

    if (doc.Histogram.Configured && doc.Histogram.Highest == 0) {
        throw std::runtime_error("runtime.histogram.highest must be greater than zero");
    }
    ValidateNonNegative(doc.RetryInitialBackoffMs, "runtime.retry.initial_backoff_ms");
    ValidateNonNegative(doc.RetryMaxBackoffMs, "runtime.retry.max_backoff_ms");
    if (doc.RetryMaxBackoffMs < doc.RetryInitialBackoffMs) {
        throw std::runtime_error(
            "runtime.retry.max_backoff_ms must be greater than or equal to initial_backoff_ms");
    }
    if (doc.RetryJitter != "none" && doc.RetryJitter != "full") {
        throw std::runtime_error("runtime.retry.jitter must be \"none\" or \"full\"");
    }

    ValidateYdbAuth(doc);
}

} // anonymous

TRunConfigDocument LoadRunConfigDocument(const std::string& path) {
    const std::string raw = ReadFile(path);
    Json root = Json::parse(raw);
    TRunConfigDocument doc;
    doc.RunConfigSha256 = ComputeBytesSha256Hex(raw);
    doc.SchemaVersion = root.value("schema_version", 0);
    doc.RunId = root.value("run_id", "");
    doc.ProfileName = root.value("profile_name", "");
    doc.Binary = root.value("binary", "");
    // Work dir is the directory that contains the distributed run-config.json.
    doc.RunDir = ParentDir(path);

    if (root.contains("database") && root["database"].is_object()) {
        const auto& db = root["database"];
        doc.Dbms = db.value("dbms", "");
        doc.Endpoint = db.value("endpoint", "");
        doc.Database = db.value("database", "");
        doc.Path = db.value("path", "");
        doc.AuthScheme = db.value("auth_scheme", "");
        doc.User = db.value("user", "");
        doc.PasswordEnv = db.value("password_env", "");
        doc.TokenEnv = db.value("token_env", "");
        doc.SaKeyFile = db.value("sa_key_file", "");
        doc.CaFile = db.value("ca_file", "");
        if (db.contains("options")) {
            if (!db["options"].is_object()) {
                throw std::runtime_error("database.options must be an object");
            }
            // YDB currently accepts no adapter options here; reject unknowns.
            for (const auto& item : db["options"].items()) {
                throw std::runtime_error(
                    "unknown database.options." + item.key() + " for dbms=ydb");
            }
        }
    }
    if (root.contains("scale") && root["scale"].is_object()) {
        doc.ScaleWarehouses = ReadIntNonNegative(
            root["scale"], "warehouses", 0, "scale.warehouses");
    }
    if (root.contains("data") && root["data"].is_object()) {
        const auto& data = root["data"];
        doc.BatchRows = ReadIntNonNegative(data, "batch_rows", 0, "data.batch_rows");
        if (data.contains("seed")) {
            doc.HasSeed = true;
            doc.Seed = data.value("seed", static_cast<int64_t>(0));
        }
    }
    if (root.contains("phases") && root["phases"].is_object()) {
        const auto& phases = root["phases"];
        doc.PhasePolicy.StartLeadMs = ReadInt64(phases, "start_lead_ms", 0, "phases.start_lead_ms");
        doc.PhasePolicy.RampUpMs = ReadInt64(phases, "ramp_up_ms", 0, "phases.ramp_up_ms");
        doc.PhasePolicy.MeasurementMs = ReadInt64(phases, "measurement_ms", 0, "phases.measurement_ms");
        doc.PhasePolicy.TransactionDrainMs = ReadInt64(
            phases, "transaction_drain_ms", 0, "phases.transaction_drain_ms");
        doc.PhasePolicy.StopGraceMs = ReadInt64(phases, "stop_grace_ms", 0, "phases.stop_grace_ms");
        doc.PhasePolicy.MaxClockSkewMs = ReadInt64(
            phases, "max_clock_skew_ms", 0, "phases.max_clock_skew_ms");
    }
    if (root.contains("runtime") && root["runtime"].is_object()) {
        const auto& rt = root["runtime"];
        const auto pacing = rt.value("pacing", std::string("enabled"));
        if (pacing != "enabled" && pacing != "disabled") {
            throw std::runtime_error("runtime.pacing must be \"enabled\" or \"disabled\"");
        }
        doc.PacingEnabled = pacing == "enabled";
        if (rt.contains("think_time_distribution")) {
            const auto dist = rt.value("think_time_distribution", std::string("exponential"));
            if (!ParseThinkTimeDistribution(dist, doc.ThinkTimeDistribution)) {
                throw std::runtime_error(
                    "runtime.think_time_distribution must be \"exponential\", "
                    "\"compatibility\", or \"constant\"");
            }
        }
        if (rt.contains("retry") && rt["retry"].is_object()) {
            const auto& retry = rt["retry"];
            doc.RetryMaxAttempts = ReadSizeTNonNegative(
                retry, "max_attempts", 0, "runtime.retry.max_attempts");
            doc.RetryInitialBackoffMs = ReadInt64(
                retry, "initial_backoff_ms", 10, "runtime.retry.initial_backoff_ms");
            doc.RetryMaxBackoffMs = ReadInt64(
                retry, "max_backoff_ms", 500, "runtime.retry.max_backoff_ms");
            doc.RetryJitter = retry.value("jitter", std::string("full"));
            doc.RetryAmbiguousCommit = retry.value("retry_ambiguous_commit", false);
        }
        if (rt.contains("histogram") && rt["histogram"].is_object()) {
            const auto& h = rt["histogram"];
            doc.Histogram.Configured = true;
            doc.Histogram.Unit = h.value("unit", "ms");
            const int64_t highest = ReadInt64(
                h, "highest", static_cast<int64_t>(32768), "runtime.histogram.highest");
            if (highest < 0) {
                throw std::runtime_error("runtime.histogram.highest must not be negative");
            }
            doc.Histogram.Highest = static_cast<uint64_t>(highest);
            if (doc.Histogram.Unit != "ms" && doc.Histogram.Unit != "us") {
                throw std::runtime_error("runtime.histogram.unit must be \"ms\" or \"us\"");
            }
            if (h.contains("lowest") || h.contains("significant_figures")) {
                throw std::runtime_error(
                    "runtime.histogram.lowest and significant_figures are not supported; "
                    "linear_exp layout uses unit and highest only "
                    "(hdr_till is an implementation default published in artifacts)");
            }
        }
    }
    if (root.contains("workload") && root["workload"].is_object()) {
        const auto& wl = root["workload"];
        doc.Workload = MakeDefaultWorkloadConfig();
        if (wl.contains("terminals_per_warehouse")) {
            doc.Workload.TerminalsPerWarehouse = ReadSizeTNonNegative(
                wl, "terminals_per_warehouse", 0, "workload.terminals_per_warehouse");
        }
        if (wl.contains("transaction_mix") && wl["transaction_mix"].is_object()) {
            const auto& mix = wl["transaction_mix"];
            doc.Workload.HasCustomMix = true;
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::NewOrder)].Weight =
                mix.value("new_order", NEW_ORDER_WEIGHT);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Payment)].Weight =
                mix.value("payment", PAYMENT_WEIGHT);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::OrderStatus)].Weight =
                mix.value("order_status", ORDER_STATUS_WEIGHT);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Delivery)].Weight =
                mix.value("delivery", DELIVERY_WEIGHT);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::StockLevel)].Weight =
                mix.value("stock_level", STOCK_LEVEL_WEIGHT);
        }
        if (wl.contains("keying_time_ms") && wl["keying_time_ms"].is_object()) {
            const auto& k = wl["keying_time_ms"];
            doc.Workload.HasCustomKeying = true;
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::NewOrder)].KeyingTimeMs =
                k.value("new_order", NEW_ORDER_KEYING_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Payment)].KeyingTimeMs =
                k.value("payment", PAYMENT_KEYING_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::OrderStatus)].KeyingTimeMs =
                k.value("order_status", ORDER_STATUS_KEYING_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Delivery)].KeyingTimeMs =
                k.value("delivery", DELIVERY_KEYING_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::StockLevel)].KeyingTimeMs =
                k.value("stock_level", STOCK_LEVEL_KEYING_TIME.count() * 1000);
        }
        if (wl.contains("think_time_ms") && wl["think_time_ms"].is_object()) {
            const auto& t = wl["think_time_ms"];
            doc.Workload.HasCustomThink = true;
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::NewOrder)].ThinkTimeMs =
                t.value("new_order", NEW_ORDER_THINK_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Payment)].ThinkTimeMs =
                t.value("payment", PAYMENT_THINK_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::OrderStatus)].ThinkTimeMs =
                t.value("order_status", ORDER_STATUS_THINK_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::Delivery)].ThinkTimeMs =
                t.value("delivery", DELIVERY_THINK_TIME.count() * 1000);
            doc.Workload.PerTx[static_cast<size_t>(ETransactionType::StockLevel)].ThinkTimeMs =
                t.value("stock_level", STOCK_LEVEL_THINK_TIME.count() * 1000);
        }
    }
    if (root.contains("load_assignment") && root["load_assignment"].is_array()) {
        for (const auto& item : root["load_assignment"]) {
            TLoaderAssignment a;
            a.Instance = item.value("instance", "");
            a.Host = item.value("host", "");
            a.OwnsGlobalData = item.value("owns_global_data", false);
            a.WarehouseRanges = ParseWarehouseRanges(item["warehouse_ranges"]);
            doc.LoadAssignments.push_back(std::move(a));
        }
    }
    if (root.contains("worker_assignment") && root["worker_assignment"].is_array()) {
        for (const auto& item : root["worker_assignment"]) {
            TWorkerAssignment a;
            a.Instance = item.value("instance", "");
            a.Host = item.value("host", "");
            a.Threads = ReadSizeTNonNegative(item, "threads", 0, "worker_assignment.threads");
            a.MaxInflight = ReadSizeTNonNegative(
                item, "max_inflight", 0, "worker_assignment.max_inflight");
            a.WarehouseRanges = ParseWarehouseRanges(item["warehouse_ranges"]);
            doc.WorkerAssignments.push_back(std::move(a));
        }
    }
    ValidateRunConfigDocument(doc, root);
    return doc;
}

TYdbConnectionConfig BuildYdbConnectionConfig(const TRunConfigDocument& doc) {
    if (doc.Endpoint.empty()) {
        throw std::runtime_error("database.endpoint is required");
    }
    if (doc.Database.empty()) {
        throw std::runtime_error("database.database is required");
    }
    TYdbConnectionConfig config;
    config.Endpoint = doc.Endpoint;
    config.Database = doc.Database;
    config.Path = doc.Path;
    config.AuthScheme = ResolveAuthScheme(doc);
    config.User = doc.User;
    config.PasswordEnv = doc.PasswordEnv;
    config.TokenEnv = doc.TokenEnv;
    config.SaKeyFile = ResolveUnderRunDir(doc.SaKeyFile, doc.RunDir);
    config.CaFile = ResolveUnderRunDir(doc.CaFile, doc.RunDir);
    return config;
}

TLoaderAssignment FindLoaderAssignment(const TRunConfigDocument& doc, const std::string& instance) {
    for (const auto& a : doc.LoadAssignments) {
        if (a.Instance == instance) {
            return a;
        }
    }
    throw std::runtime_error("loader instance not found in run-config: " + instance);
}

TWorkerAssignment FindWorkerAssignment(const TRunConfigDocument& doc, const std::string& instance) {
    for (const auto& a : doc.WorkerAssignments) {
        if (a.Instance == instance) {
            return a;
        }
    }
    throw std::runtime_error("worker instance not found in run-config: " + instance);
}

std::string InstanceWorkDir(const TRunConfigDocument& doc, const std::string& role, const std::string& instance) {
    if (doc.RunDir.empty()) {
        throw std::runtime_error("run_dir is empty");
    }
    return doc.RunDir + "/" + role + "/" + instance;
}

std::string ComputeBytesSha256Hex(const std::string& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLen = 0;
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data.data(), data.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, hash, &hashLen) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 digest failed");
    }
    EVP_MD_CTX_free(ctx);
    return HexEncode(hash, hashLen);
}

std::string ComputeFileSha256Hex(const std::string& path) {
    return ComputeBytesSha256Hex(ReadFile(path));
}

} // namespace NTpcc
