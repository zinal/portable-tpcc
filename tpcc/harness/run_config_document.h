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

    // YDB auth fields (empty for PostgreSQL).
    std::string AuthScheme;
    std::string User;
    std::string TokenEnv;
    std::string SaKeyFile;
    std::string CaFile;

    // PostgreSQL physical options (ignored by other adapters).
    // partitioning: "none" | "warehouse_hash"
    std::string Partitioning = "none";
    // Hash modulus; 0 = derive from ScaleWarehouses when partitioning=warehouse_hash.
    int PartitionCount = 0;

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
