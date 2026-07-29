#pragma once

#include "phase_policy.h"
#include "warehouse_range.h"

#include <cstdint>
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
    std::string Mode;
    std::string RunDir;
    std::string SpecStatePath;
    std::string ProfileSha256;
    std::string RunConfigSha256;
    std::string SpecStateSha256;
    std::string WorkerBinarySha256;
    std::string Edition;

    std::string Dbms;
    std::string Endpoint;
    std::string Database;
    std::string Path;
    std::string PasswordEnv;

    int ScaleWarehouses = 0;
    std::string LoadId;
    std::string LoadPlanPath;

    TPhasePolicy PhasePolicy;
    bool PacingEnabled = true;
    size_t RetryMaxAttempts = 0;

    std::vector<TLoaderAssignment> LoadAssignments;
    std::vector<TWorkerAssignment> WorkerAssignments;
};

TRunConfigDocument LoadRunConfigDocument(const std::string& path);
std::string BuildPgConnectionString(const TRunConfigDocument& doc);
TLoaderAssignment FindLoaderAssignment(const TRunConfigDocument& doc, const std::string& instance);
TWorkerAssignment FindWorkerAssignment(const TRunConfigDocument& doc, const std::string& instance);
std::string InstanceWorkDir(const TRunConfigDocument& doc, const std::string& role, const std::string& instance);
std::string ComputeFileSha256Hex(const std::string& path);
std::string ComputeBytesSha256Hex(const std::string& data);

} // namespace NTpcc
