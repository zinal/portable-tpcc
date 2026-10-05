#include "worker_loader.h"

#include "clock_calibration.h"
#include "dummy_admin_adapter.h"
#include "dummy_error_classifier.h"
#include "dummy_session.h"
#include "import.h"
#include "run_config.h"
#include "runner.h"

#include <debug_probe.h>
#include <orchestrated_roles.h>
#include <log.h>
#include <warehouse_range.h>

#include <iostream>
#include <stdexcept>

namespace NTpcc {

namespace {

const TAdapterIdentity kDummyIdentity{"dummy", "tpcc-dummy"};

} // anonymous

int RunLoaderFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<int>& threadOverride)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TLoaderRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument&) {
        return MeasureClockCalibration();
    };
    hooks.Import = [](const TRunConfigDocument& d, const TLoaderAssignment& assign) {
        TImportConfig importCfg;
        importCfg.WarehouseRanges = assign.WarehouseRanges;
        importCfg.OwnsGlobalData = assign.OwnsGlobalData;
        importCfg.TotalWarehouses = d.ScaleWarehouses;
        importCfg.WarehouseCount = CountWarehouses(assign.WarehouseRanges);
        importCfg.BatchRows = d.BatchRows;
        if (d.HasSeed) {
            importCfg.Seed = static_cast<uint64_t>(d.Seed);
        }
        importCfg.RunId = d.RunId;
        ImportSync(importCfg);
    };
    return RunOrchestratedLoader(doc, instance, kDummyIdentity, hooks, threadOverride);
}

int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const std::optional<int>& threadOverride,
    const std::optional<int>& maxInflightOverride,
    const std::optional<int>& metricsPort)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    TWorkerRoleHooks hooks;
    hooks.Calibrate = [](const TRunConfigDocument&) {
        return MeasureClockCalibration();
    };
    hooks.Run = [instance, metricsPort](
        const TRunConfigDocument& d,
        const TWorkerAssignment& assign,
        const std::string& instanceDir,
        std::chrono::system_clock::time_point startAt,
        TTerminalStats& aggregated)
    {
        TRunConfig runCfg;
        runCfg.Delay = DelayConfigFromDocument(d);
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
        if (metricsPort.has_value()) {
            runCfg.MetricsPort = *metricsPort;
        }
        runCfg.ThinkTimeDistribution = d.ThinkTimeDistribution;
        runCfg.StartAt = startAt;
        return RunSync(runCfg, &aggregated);
    };
    return RunOrchestratedWorker(
        doc, instance, startAtRfc3339, kDummyIdentity, hooks, threadOverride, maxInflightOverride);
}

int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedSchema(doc, instance, [instance](const TRunConfigDocument&) {
        TDummyAdminAdapter admin;
        admin.EnsureSchema();
        auto desc = admin.Describe();
        LOG_I("Schema ready (server=" << desc.ServerVersion << ", client=" << desc.ClientVersion
              << ", instance=" << instance << ")");
    });
}

int RunIndexesFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedIndexes(doc, instance, [instance](const TRunConfigDocument&) {
        TDummyAdminAdapter admin;
        admin.EnsureIndexes();
        admin.EnsureStatistics();
        LOG_I("Indexes and statistics ready (instance=" << instance << ")");
    });
}

int RunDropFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    return RunOrchestratedDrop(doc, instance, [instance](const TRunConfigDocument&) {
        TDummyAdminAdapter admin;
        admin.Clean();
        LOG_I("Drop complete (instance=" << instance << ")");
    });
}

namespace {

TDebugReport RunDummyDebugProbe(const TDummyDelayConfig& delay, TDebugProbeRequest req) {
    TDummySessionFactory factory(delay, 1);
    TDummyErrorClassifier classifier;
    req.SessionFactory = &factory;
    req.ErrorClassifier = &classifier;
    req.Isolation = EIsolationLevel::RepeatableRead;
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
            return RunDummyDebugProbe(DelayConfigFromDocument(d), req);
        });
}

void DebugSync(const TDummyDelayConfig& delay, int warehouseCount, int repeats) {
    TDebugProbeRequest req;
    req.WarehouseID = 1;
    req.WarehouseCount = warehouseCount > 0 ? static_cast<size_t>(warehouseCount) : 1;
    req.DistrictID = 1;
    req.Repeats = repeats;
    auto report = RunDummyDebugProbe(delay, req);
    WriteDebugReportJson("debug.json", report);
    std::cout << "Debug report written to debug.json" << std::endl;
    if (!report.Ok) {
        throw std::runtime_error("debug probe recorded failed transactions");
    }
}

} // namespace NTpcc
