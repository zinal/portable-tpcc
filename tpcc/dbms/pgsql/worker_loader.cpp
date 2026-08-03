#include "worker_loader.h"

#include "pg_admin_adapter.h"
#include "clock_calibration.h"
#include "import.h"
#include "path_checker.h"
#include "run_config.h"
#include "runner.h"

#include <orchestrated_roles.h>
#include <log.h>
#include <warehouse_range.h>

namespace NTpcc {

namespace {

const TAdapterIdentity kPgIdentity{"pgsql", "tpcc-pgsql"};

} // anonymous

int RunLoaderFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TLoaderRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument& d) {
        const std::string connection = BuildPgConnectionString(d);
        CheckDbForImport(connection, d.Path);
        return MeasureClockCalibration(connection, d.Endpoint);
    };
    hooks.Import = [](const TRunConfigDocument& d, const TLoaderAssignment& assign) {
        const std::string connection = BuildPgConnectionString(d);
        TImportConfig importCfg;
        importCfg.ConnectionString = connection;
        importCfg.Path = d.Path;
        importCfg.WarehouseRanges = assign.WarehouseRanges;
        importCfg.OwnsGlobalData = assign.OwnsGlobalData;
        importCfg.TotalWarehouses = d.ScaleWarehouses;
        importCfg.LoadThreadCount = 0;
        importCfg.BatchRows = d.BatchRows;
        if (d.HasSeed) {
            importCfg.Seed = static_cast<uint64_t>(d.Seed);
        }
        importCfg.RunId = d.RunId;
        ImportSync(importCfg);
    };
    return RunOrchestratedLoader(doc, instance, kPgIdentity, hooks);
}

int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TWorkerRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument& d) {
        const std::string connection = BuildPgConnectionString(d);
        CheckDbForRun(connection, d.ScaleWarehouses, d.Path);
        return MeasureClockCalibration(connection, d.Endpoint);
    };
    hooks.Run = [instance](
        const TRunConfigDocument& d,
        const TWorkerAssignment& assign,
        const std::string& instanceDir,
        std::chrono::system_clock::time_point startAt,
        TTerminalStats& aggregated)
    {
        TRunConfig runCfg;
        runCfg.ConnectionString = BuildPgConnectionString(d);
        runCfg.Path = d.Path;
        runCfg.WarehouseRanges = assign.WarehouseRanges;
        runCfg.WarehouseCount = CountWarehouses(assign.WarehouseRanges);
        runCfg.ScaleWarehouses = d.ScaleWarehouses;
        runCfg.ThreadCount = assign.Threads;
        runCfg.MaxInflight = assign.MaxInflight;
        runCfg.NoDelays = !d.PacingEnabled;
        runCfg.Orchestrated = true;
        runCfg.PhasePolicy = d.PhasePolicy;
        runCfg.Instance = instance;
        runCfg.InstanceDir = instanceDir;
        runCfg.RetryMaxAttempts = d.RetryMaxAttempts;
        runCfg.RetryInitialBackoffMs = d.RetryInitialBackoffMs;
        runCfg.RetryMaxBackoffMs = d.RetryMaxBackoffMs;
        runCfg.RetryJitter = d.RetryJitter;
        runCfg.RetryAmbiguousCommit = d.RetryAmbiguousCommit;
        runCfg.Workload = d.Workload;
        runCfg.Histogram = d.Histogram;
        runCfg.ThinkTimeDistribution = d.ThinkTimeDistribution;
        runCfg.StartAt = startAt;
        return RunSync(runCfg, &aggregated);
    };
    return RunOrchestratedWorker(doc, instance, startAtRfc3339, kPgIdentity, hooks);
}

int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedSchema(doc, instance, [instance](const TRunConfigDocument& d) {
        const std::string connection = BuildPgConnectionString(d);
        CheckDbForInit(connection, d.Path);
        TPgAdminAdapter admin(connection, d.Path);
        admin.EnsureSchema();
        auto desc = admin.Describe();
        LOG_I("Schema ready (server=" << desc.ServerVersion << ", client=" << desc.ClientVersion
              << ", instance=" << instance << ")");
    });
}

} // namespace NTpcc
