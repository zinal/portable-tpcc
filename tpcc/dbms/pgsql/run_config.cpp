#include "run_config.h"

#include <think_time.h>

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
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
        doc.PasswordEnv = db.value("password_env", "");
        if (db.contains("options")) {
            if (!db["options"].is_object()) {
                throw std::runtime_error("database.options must be an object");
            }
            // PostgreSQL currently accepts no adapter options; reject unknowns.
            for (const auto& item : db["options"].items()) {
                throw std::runtime_error(
                    "unknown database.options." + item.key() + " for dbms=pgsql");
            }
        }
    }
    if (root.contains("scale") && root["scale"].is_object()) {
        doc.ScaleWarehouses = root["scale"].value("warehouses", 0);
    }
    if (root.contains("data") && root["data"].is_object()) {
        const auto& data = root["data"];
        doc.BatchRows = data.value("batch_rows", 0);
        if (data.contains("seed")) {
            doc.HasSeed = true;
            doc.Seed = data.value("seed", static_cast<int64_t>(0));
        }
    }
    if (root.contains("phases") && root["phases"].is_object()) {
        const auto& phases = root["phases"];
        doc.PhasePolicy.StartLeadMs = phases.value("start_lead_ms", 0);
        doc.PhasePolicy.RampUpMs = phases.value("ramp_up_ms", 0);
        doc.PhasePolicy.MeasurementMs = phases.value("measurement_ms", 0);
        doc.PhasePolicy.TransactionDrainMs = phases.value("transaction_drain_ms", 0);
        doc.PhasePolicy.StopGraceMs = phases.value("stop_grace_ms", 0);
        doc.PhasePolicy.MaxClockSkewMs = phases.value("max_clock_skew_ms", 0);
    }
    if (root.contains("runtime") && root["runtime"].is_object()) {
        const auto& rt = root["runtime"];
        doc.PacingEnabled = rt.value("pacing", "enabled") == std::string("enabled");
        if (rt.contains("think_time_distribution")) {
            const auto dist = rt.value("think_time_distribution", std::string("exponential"));
            if (!ParseThinkTimeDistribution(dist, doc.ThinkTimeDistribution)) {
                throw std::runtime_error(
                    "runtime.think_time_distribution must be \"exponential\", "
                    "\"compatibility\", or \"constant\"");
            }
        }
        if (rt.contains("retry") && rt["retry"].is_object()) {
            doc.RetryMaxAttempts = rt["retry"].value("max_attempts", 0);
            doc.RetryAmbiguousCommit = rt["retry"].value("retry_ambiguous_commit", false);
        }
        if (rt.contains("histogram") && rt["histogram"].is_object()) {
            const auto& h = rt["histogram"];
            doc.Histogram.Configured = true;
            doc.Histogram.Unit = h.value("unit", "ms");
            doc.Histogram.Highest = h.value("highest", static_cast<uint64_t>(32768));
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
            doc.Workload.TerminalsPerWarehouse = wl.value("terminals_per_warehouse", 0);
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
            a.Threads = item.value("threads", 0);
            a.MaxInflight = item.value("max_inflight", 0);
            a.WarehouseRanges = ParseWarehouseRanges(item["warehouse_ranges"]);
            doc.WorkerAssignments.push_back(std::move(a));
        }
    }
    if (doc.RunId.empty()) {
        throw std::runtime_error("run-config missing run_id");
    }
    if (doc.Dbms != "pgsql") {
        throw std::runtime_error("tpcc-pgsql run-config requires database.dbms=pgsql");
    }
    return doc;
}

std::string BuildPgConnectionString(const TRunConfigDocument& doc) {
    if (doc.PasswordEnv.empty()) {
        throw std::runtime_error("database.password_env is required");
    }
    const char* password = std::getenv(doc.PasswordEnv.c_str());
    if (!password) {
        throw std::runtime_error("environment variable not set: " + doc.PasswordEnv);
    }

    const auto& ep = doc.Endpoint;
    if (ep.empty()) {
        throw std::runtime_error("database.endpoint is required");
    }
    // Reject libpq keyword strings so secrets cannot bypass password_env.
    if (ep.find('=') != std::string::npos) {
        throw std::runtime_error(
            "database.endpoint must be host or host:port; "
            "libpq keyword strings are not accepted (use password_env for secrets)");
    }

    std::string host;
    std::string port;
    auto colon = ep.find(':');
    if (colon == std::string::npos) {
        host = ep;
        port = "5432";
    } else {
        host = ep.substr(0, colon);
        port = ep.substr(colon + 1);
    }
    if (host.empty() || port.empty()) {
        throw std::runtime_error("database.endpoint must be host or host:port");
    }

    std::string user = "postgres";
    if (const char* u = std::getenv("TPCC_PG_USER")) {
        user = u;
    }

    return "host=" + host + " port=" + port + " dbname=" + doc.Database + " user=" + user + " password=" + password;
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
