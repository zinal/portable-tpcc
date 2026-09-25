#include "worker_loader.h"

#include "ydb_admin_adapter.h"
#include "clock_calibration.h"
#include "import.h"
#include "path_checker.h"
#include "run_config.h"
#include "runner.h"
#include "ydb_error_classifier.h"
#include "ydb_session.h"
#include "ydb_tx_mode.h"

#include <debug_probe.h>
#include <orchestrated_roles.h>
#include <log.h>
#include <warehouse_range.h>

#include <iostream>
#include <stdexcept>

namespace NTpcc {

namespace {

const TAdapterIdentity kYdbIdentity{"ydb", "tpcc-ydb"};

} // anonymous

int RunLoaderFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& threadOverride)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TLoaderRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument& d) {
        const auto connection = BuildYdbConnectionConfig(d);
        CheckDbForImport(connection);
        return MeasureClockCalibration(connection, d.Endpoint);
    };
    hooks.Import = [](const TRunConfigDocument& d, const TLoaderAssignment& assign) {
        TImportConfig importCfg;
        importCfg.Connection = BuildYdbConnectionConfig(d);
        importCfg.WarehouseRanges = assign.WarehouseRanges;
        importCfg.OwnsGlobalData = assign.OwnsGlobalData;
        importCfg.TotalWarehouses = d.ScaleWarehouses;
        importCfg.LoadThreadCount = assign.Threads;
        importCfg.BatchRows = d.BatchRows;
        if (d.HasSeed) {
            importCfg.Seed = static_cast<uint64_t>(d.Seed);
        }
        importCfg.RunId = d.RunId;
        ImportSync(importCfg);
    };
    return RunOrchestratedLoader(doc, instance, kYdbIdentity, hooks, threadOverride);
}

int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const std::optional<int>& threadOverride,
    const std::optional<int>& maxInflightOverride)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TWorkerRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument& d) {
        const auto connection = BuildYdbConnectionConfig(d);
        CheckDbForRun(connection, d.ScaleWarehouses);
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
        runCfg.Connection = BuildYdbConnectionConfig(d);
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
        runCfg.StatsInterval = StatsIntervalFromMs(d.StatsIntervalMs);
        runCfg.ThinkTimeDistribution = d.ThinkTimeDistribution;
        runCfg.StartAt = startAt;
        if (!ParseYdbTxMode(d.TxMode, runCfg.Isolation)) {
            throw std::runtime_error(
                "database.options.tx_mode must be \"snapshot-rw\" or \"serializable-rw\"");
        }
        return RunSync(runCfg, &aggregated);
    };
    return RunOrchestratedWorker(
        doc, instance, startAtRfc3339, kYdbIdentity, hooks, threadOverride, maxInflightOverride);
}

int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedSchema(doc, instance, [instance](const TRunConfigDocument& d) {
        const auto connection = BuildYdbConnectionConfig(d);
        CheckDbForInit(connection);
        TYdbAdminAdapter admin(connection, d.ScaleWarehouses);
        admin.EnsureSchema();
        auto desc = admin.Describe();
        LOG_I("Schema ready (server=" << desc.ServerVersion << ", client=" << desc.ClientVersion
              << ", instance=" << instance << ")");
    });
}

int RunIndexesFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedIndexes(doc, instance, [instance](const TRunConfigDocument& d) {
        const auto connection = BuildYdbConnectionConfig(d);
        CheckDbForIndexes(connection);
        TYdbAdminAdapter admin(connection, d.ScaleWarehouses);
        admin.EnsureIndexes();
        admin.EnsureStatistics();
        LOG_I("Indexes and statistics ready (instance=" << instance << ")");
    });
}

int RunDropFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedDrop(doc, instance, [instance](const TRunConfigDocument& d) {
        const auto connection = BuildYdbConnectionConfig(d);
        TYdbAdminAdapter admin(connection, d.ScaleWarehouses);
        admin.Clean();
        LOG_I("Drop complete (instance=" << instance << ")");
    });
}

namespace {

TDebugReport RunYdbDebugProbe(
    const TYdbConnectionConfig& connection,
    EIsolationLevel isolation,
    TDebugProbeRequest req)
{
    TYdbConnection conn(connection);
    TYdbSessionFactory factory(conn);
    TYdbErrorClassifier classifier;
    req.SessionFactory = &factory;
    req.ErrorClassifier = &classifier;
    req.Isolation = isolation;
    return RunDebugProbe(req);
}

} // anonymous

int RunDebugFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    int repeats)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedDebug(doc, instance, repeats,
        [](const TRunConfigDocument& d, TDebugProbeRequest req) {
            const auto connection = BuildYdbConnectionConfig(d);
            CheckDbForRun(connection, d.ScaleWarehouses);
            EIsolationLevel isolation = EIsolationLevel::RepeatableRead;
            if (!ParseYdbTxMode(d.TxMode, isolation)) {
                throw std::runtime_error(
                    "database.options.tx_mode must be \"snapshot-rw\" or \"serializable-rw\"");
            }
            return RunYdbDebugProbe(connection, isolation, req);
        });
}

void DebugSync(
    const TYdbConnectionConfig& connection,
    int warehouseCount,
    int repeats,
    EIsolationLevel isolation)
{
    CheckDbForRun(connection, warehouseCount);
    TDebugProbeRequest req;
    req.WarehouseID = 1;
    req.WarehouseCount = warehouseCount > 0 ? static_cast<size_t>(warehouseCount) : 1;
    req.DistrictID = 1;
    req.Repeats = repeats;
    auto report = RunYdbDebugProbe(connection, isolation, req);
    WriteDebugReportJson("debug.json", report);
    std::cout << "Debug report written to debug.json" << std::endl;
    if (!report.Ok) {
        throw std::runtime_error("debug probe recorded failed transactions");
    }
}

} // namespace NTpcc
