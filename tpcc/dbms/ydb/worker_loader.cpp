#include "worker_loader.h"

#include "ydb_admin_adapter.h"
#include "clock_calibration.h"
#include "import.h"
#include "path_checker.h"
#include "run_config.h"
#include "runner.h"

#include <artifacts.h>
#include <log.h>
#include <time_util.h>

#include <unistd.h>

namespace NTpcc {

int RunLoaderFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    const auto assign = FindLoaderAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "loader", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "loader", static_cast<int>(::getpid()), nonce);

    const auto connection = BuildYdbConnectionConfig(doc);
    CheckDbForImport(connection);
    const auto clockCalibration = MeasureClockCalibration(connection, doc.Endpoint);
    WriteReadyJson(paths, doc, instance, assign.WarehouseRanges, nonce, clockCalibration, "ydb");

    TImportConfig importCfg;
    importCfg.Connection = connection;
    importCfg.WarehouseRanges = assign.WarehouseRanges;
    importCfg.OwnsGlobalData = assign.OwnsGlobalData;
    importCfg.TotalWarehouses = doc.ScaleWarehouses;
    importCfg.LoadThreadCount = 0;
    importCfg.BatchRows = doc.BatchRows;
    if (doc.HasSeed) {
        importCfg.Seed = static_cast<uint64_t>(doc.Seed);
    }
    importCfg.RunId = doc.RunId;

    int exitCode = 0;
    try {
        ImportSync(importCfg);
        // ImportSync creates secondary indexes and runs ANALYZE.
    } catch (const std::exception& ex) {
        LOG_E("Loader failed: " << ex.what());
        exitCode = 1;
    }

    WriteLoaderResultJson(paths, doc, instance, assign, exitCode);
    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

int RunWorkerFromRunConfig(
    const std::string& runConfigPath,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339)
{
    const auto doc = LoadRunConfigDocument(runConfigPath);
    const auto assign = FindWorkerAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "worker", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "worker", static_cast<int>(::getpid()), nonce);

    const auto connection = BuildYdbConnectionConfig(doc);
    CheckDbForRun(connection, doc.ScaleWarehouses);
    const auto clockCalibration = MeasureClockCalibration(connection, doc.Endpoint);
    WriteReadyJson(paths, doc, instance, assign.WarehouseRanges, nonce, clockCalibration, "ydb");
    if (!IsClockSkewWithinBudget(clockCalibration, doc.PhasePolicy.MaxClockSkewMs)) {
        LOG_E(instance + ": " + FormatClockSkewViolation(clockCalibration, doc.PhasePolicy.MaxClockSkewMs));
        const bool recordUs = doc.Histogram.Configured && doc.Histogram.Unit == "us";
        const uint64_t histHdr = doc.Histogram.Configured
            ? doc.Histogram.HdrTill()
            : 4096ull;
        const uint64_t histMax = doc.Histogram.Configured
            ? doc.Histogram.MaxValue()
            : 32768ull;
        TTerminalStats emptyStats(histHdr, histMax, recordUs);
        const auto now = std::chrono::system_clock::now();
        WriteWorkerResultJson(
            paths, doc, instance, assign, emptyStats, false,
            now, now, now, now, 0.0, 1, nonce, "ydb", "tpcc-ydb");
        WriteArtifactManifest(paths, instance, nonce, 1);
        return 1;
    }

    TRunConfig runCfg;
    runCfg.Connection = connection;
    runCfg.WarehouseRanges = assign.WarehouseRanges;
    runCfg.WarehouseCount = CountWarehouses(assign.WarehouseRanges);
    runCfg.ScaleWarehouses = doc.ScaleWarehouses;
    runCfg.ThreadCount = assign.Threads;
    runCfg.MaxInflight = assign.MaxInflight;
    runCfg.NoDelays = !doc.PacingEnabled;
    runCfg.Orchestrated = true;
    runCfg.PhasePolicy = doc.PhasePolicy;
    runCfg.Instance = instance;
    runCfg.InstanceDir = instanceDir;
    runCfg.RetryMaxAttempts = doc.RetryMaxAttempts;
    runCfg.RetryInitialBackoffMs = doc.RetryInitialBackoffMs;
    runCfg.RetryMaxBackoffMs = doc.RetryMaxBackoffMs;
    runCfg.RetryJitter = doc.RetryJitter;
    runCfg.RetryAmbiguousCommit = doc.RetryAmbiguousCommit;
    runCfg.Workload = doc.Workload;
    runCfg.Histogram = doc.Histogram;
    runCfg.ThinkTimeDistribution = doc.ThinkTimeDistribution;

    if (startAtRfc3339.has_value()) {
        runCfg.StartAt = ParseRfc3339Utc(*startAtRfc3339);
    } else {
        throw std::runtime_error("orchestrated worker requires --start-at=<RFC3339-UTC>");
    }

    const bool recordUs = doc.Histogram.Configured && doc.Histogram.Unit == "us";
    const uint64_t histHdr = doc.Histogram.Configured
        ? doc.Histogram.HdrTill()
        : 4096ull;
    const uint64_t histMax = doc.Histogram.Configured
        ? doc.Histogram.MaxValue()
        : 32768ull;
    TTerminalStats aggregated(histHdr, histMax, recordUs);
    TRunOutcome outcome;
    int exitCode = 0;
    try {
        outcome = RunSync(runCfg, &aggregated);
        exitCode = outcome.ExitCode;
    } catch (const std::exception& ex) {
        LOG_E("Worker failed: " << ex.what());
        exitCode = 1;
    }

    WriteWorkerResultJson(
        paths, doc, instance, assign, aggregated, outcome.HighResHistogram,
        outcome.RampStart, outcome.MeasurementStart, outcome.MeasurementEnd,
        outcome.DrainDeadline, outcome.MeasurementSeconds, exitCode, nonce,
        "ydb", "tpcc-ydb");
    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

int RunSchemaFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    const std::string instanceDir = InstanceWorkDir(doc, "schema", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "schema", static_cast<int>(::getpid()), nonce);

    const auto connection = BuildYdbConnectionConfig(doc);
    int exitCode = 0;
    try {
        CheckDbForInit(connection);
        TYdbAdminAdapter admin(connection, doc.ScaleWarehouses);
        admin.EnsureSchema();
        auto desc = admin.Describe();
        LOG_I("Schema ready (server=" << desc.ServerVersion << ", client=" << desc.ClientVersion << ", instance=" << instance << ")");
    } catch (const std::exception& ex) {
        LOG_E("Schema failed: " << ex.what());
        exitCode = 1;
    }

    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

} // namespace NTpcc
