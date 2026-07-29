#include "run_config.h"

#include <nlohmann/json.hpp>

#include <openssl/evp.h>

#include <cstdio>
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

} // anonymous

TRunConfigDocument LoadRunConfigDocument(const std::string& path) {
    const std::string raw = ReadFile(path);
    Json root = Json::parse(raw);
    TRunConfigDocument doc;
    doc.RunConfigSha256 = ComputeBytesSha256Hex(raw);
    doc.SchemaVersion = root.value("schema_version", 0);
    doc.RunId = root.value("run_id", "");
    doc.Mode = root.value("mode", "");
    if (root.contains("profile") && root["profile"].is_object()) {
        doc.ProfileSha256 = root["profile"].value("sha256", "");
    }
    if (root.contains("spec") && root["spec"].is_object()) {
        const auto& spec = root["spec"];
        doc.Edition = spec.value("edition", "");
        doc.SpecStateSha256 = spec.value("state_sha256", "");
    }
    if (root.contains("artifacts") && root["artifacts"].is_object()) {
        doc.WorkerBinarySha256 = root["artifacts"].value("worker_binary_sha256", "");
    }
    if (root.contains("paths") && root["paths"].is_object()) {
        const auto& paths = root["paths"];
        doc.RunDir = paths.value("run_dir", "");
        doc.SpecStatePath = paths.value("spec_state", "");
        doc.LoadPlanPath = paths.value("load_plan", "");
    }
    if (root.contains("database") && root["database"].is_object()) {
        const auto& db = root["database"];
        doc.Dbms = db.value("dbms", "");
        doc.Endpoint = db.value("endpoint", "");
        doc.Database = db.value("database", "");
        doc.Path = db.value("path", "");
        doc.PasswordEnv = db.value("password_env", "");
    }
    if (root.contains("scale") && root["scale"].is_object()) {
        doc.ScaleWarehouses = root["scale"].value("warehouses", 0);
    }
    if (root.contains("load") && root["load"].is_object()) {
        doc.LoadId = root["load"].value("load_id", "");
    }
    if (root.contains("phase_policy") && root["phase_policy"].is_object()) {
        const auto& pp = root["phase_policy"];
        doc.PhasePolicy.StartLeadMs = pp.value("start_lead_ms", 0);
        doc.PhasePolicy.RampUpMs = pp.value("ramp_up_ms", 0);
        doc.PhasePolicy.MeasurementMs = pp.value("measurement_ms", 0);
        doc.PhasePolicy.TransactionDrainMs = pp.value("transaction_drain_ms", 0);
        doc.PhasePolicy.StopGraceMs = pp.value("stop_grace_ms", 0);
    }
    if (root.contains("runtime") && root["runtime"].is_object()) {
        const auto& rt = root["runtime"];
        doc.PacingEnabled = rt.value("pacing", "enabled") == std::string("enabled");
        if (rt.contains("retry") && rt["retry"].is_object()) {
            doc.RetryMaxAttempts = rt["retry"].value("max_attempts", 0);
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
    if (doc.RunId.empty() || doc.RunDir.empty()) {
        throw std::runtime_error("run-config missing run_id or paths.run_dir");
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

    std::string host;
    std::string port;
    const auto& ep = doc.Endpoint;
    if (ep.find('=') != std::string::npos) {
        return ep;
    }
    auto colon = ep.find(':');
    if (colon == std::string::npos) {
        host = ep;
        port = "5432";
    } else {
        host = ep.substr(0, colon);
        port = ep.substr(colon + 1);
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
