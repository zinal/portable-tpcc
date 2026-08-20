#pragma once

#include <phase_policy.h>
#include <warehouse_range.h>
#include <workload_config.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace NTpcc {

struct TLoaderAssignment {
    std::string Instance;
    std::string Host;
    std::vector<TWarehouseRange> WarehouseRanges;
    bool OwnsGlobalData = false;
    // Import concurrency; 0 means auto (CPU/warehouse capped) in ImportSync.
    size_t Threads = 0;
};

struct TWorkerAssignment {
    std::string Instance;
    std::string Host;
    std::vector<TWarehouseRange> WarehouseRanges;
    size_t Threads = 0;
    size_t MaxInflight = 0;
};

struct TRunConfigDocument {
    int SchemaVersion = 0;
    std::string RunId;
    std::string ProfileName;
    std::string Binary;
    std::string RunDir;
    std::string RunConfigSha256;

    std::string Dbms;
    std::string Endpoint;
    std::string Database;
    std::string Path;
    std::string PasswordEnv;
    // Worker-local password file (orchestrator deploy); preferred over PasswordEnv.
    std::string PasswordFile;

    // Auth fields: User is used by PostgreSQL, YDB login, and OceanBase;
    // others are YDB-only.
    std::string AuthScheme;
    std::string User;
    std::string TokenEnv;
    std::string SaKeyFile;
    std::string CaFile;

    // PostgreSQL/OceanBase physical options (ignored by other adapters).
    // partitioning: PG "none" | "warehouse_hash"; OB "none" | "tablegroup_hash"
    std::string Partitioning = "none";
    // Hash modulus; 0 = derive from ScaleWarehouses when hashing.
    int PartitionCount = 0;
    bool ForeignKeys = true;
    // OceanBase session query timeout for bulk load / index / DBMS_STATS (seconds).
    // Emitted as connection property query_timeout; 0 means use adapter default.
    int QueryTimeoutSeconds = 0;
    // OceanBase CREATE INDEX degree of parallelism (PARALLEL n).
    // 0 means use adapter default (OB_DEFAULT_INDEX_PARALLEL); 1 = serial.
    int IndexParallel = 0;

    int ScaleWarehouses = 0;
    int64_t Seed = 0;
    bool HasSeed = false;
    int BatchRows = 0;

    TPhasePolicy PhasePolicy;
    bool PacingEnabled = true;
    EThinkTimeDistribution ThinkTimeDistribution = EThinkTimeDistribution::Exponential;
    size_t RetryMaxAttempts = 0;
    int64_t RetryInitialBackoffMs = 10;
    int64_t RetryMaxBackoffMs = 500;
    std::string RetryJitter = "full";
    bool RetryAmbiguousCommit = false;

    TWorkloadConfig Workload = MakeDefaultWorkloadConfig();
    THistogramConfig Histogram;

    std::vector<TLoaderAssignment> LoadAssignments;
    std::vector<TWorkerAssignment> WorkerAssignments;
};

inline TLoaderAssignment FindLoaderAssignment(const TRunConfigDocument& doc, const std::string& instance) {
    for (const auto& a : doc.LoadAssignments) {
        if (a.Instance == instance) {
            return a;
        }
    }
    throw std::runtime_error("loader instance not found in run-config: " + instance);
}

inline TWorkerAssignment FindWorkerAssignment(const TRunConfigDocument& doc, const std::string& instance) {
    for (const auto& a : doc.WorkerAssignments) {
        if (a.Instance == instance) {
            return a;
        }
    }
    throw std::runtime_error("worker instance not found in run-config: " + instance);
}

inline std::string InstanceWorkDir(const TRunConfigDocument& doc, const std::string& role, const std::string& instance) {
    if (doc.RunDir.empty()) {
        throw std::runtime_error("run_dir is empty");
    }
    return doc.RunDir + "/" + role + "/" + instance;
}

} // namespace NTpcc
