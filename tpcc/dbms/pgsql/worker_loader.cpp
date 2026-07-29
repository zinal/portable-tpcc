#include "worker_loader.h"

#include "artifacts.h"
#include "import.h"
#include "init.h"
#include "path_checker.h"
#include "run_config.h"
#include "runner.h"

#include <log.h>

#include <unistd.h>

namespace NTpcc {

int RunLoaderFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    const auto assign = FindLoaderAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "loader", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);

    WriteProcessJson(paths, doc, instance, "loader", static_cast<int>(::getpid()));
    WriteReadyJson(paths, doc, instance, assign.WarehouseRanges);

    const std::string connection = BuildPgConnectionString(doc);
    CheckDbForImport(connection, doc.Path);

    TImportConfig importCfg;
    importCfg.ConnectionString = connection;
    importCfg.Path = doc.Path;
    importCfg.WarehouseRanges = assign.WarehouseRanges;
    importCfg.OwnsGlobalData = assign.OwnsGlobalData;
    importCfg.TotalWarehouses = doc.ScaleWarehouses;
    importCfg.LoadThreadCount = 0;
    importCfg.UseTui = false;

    int exitCode = 0;
    try {
        ImportSync(importCfg);
        CreateIndexes(connection, doc.Path);
    } catch (const std::exception& ex) {
        LOG_E("Loader failed: {}", ex.what());
        exitCode = 1;
    }

    WriteLoaderResultJson(paths, doc, instance, assign, exitCode);
    WriteArtifactManifest(paths, instance, GenerateInstanceNonce(), exitCode);
    return exitCode;
}

int RunWorkerFromRunConfig(const std::string& runConfigPath, const std::string& instance) {
    const auto doc = LoadRunConfigDocument(runConfigPath);
    const auto assign = FindWorkerAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "worker", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);

    WriteProcessJson(paths, doc, instance, "worker", static_cast<int>(::getpid()));
    WriteReadyJson(paths, doc, instance, assign.WarehouseRanges);

    const std::string connection = BuildPgConnectionString(doc);
    CheckDbForRun(connection, doc.ScaleWarehouses, doc.Path);

    TRunConfig runCfg;
    runCfg.ConnectionString = connection;
    runCfg.Path = doc.Path;
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
    runCfg.UseTui = false;

    TTerminalStats aggregated(false);
    TRunOutcome outcome;
    int exitCode = 0;
    try {
        outcome = RunSync(runCfg, &aggregated);
        exitCode = outcome.ExitCode;
    } catch (const std::exception& ex) {
        LOG_E("Worker failed: {}", ex.what());
        exitCode = 1;
    }

    WriteWorkerResultJson(
        paths, doc, instance, assign, aggregated, outcome.HighResHistogram,
        outcome.RampStart, outcome.MeasurementStart, outcome.MeasurementEnd,
        outcome.MeasurementSeconds, exitCode);
    WriteArtifactManifest(paths, instance, GenerateInstanceNonce(), exitCode);
    return exitCode;
}

} // namespace NTpcc
