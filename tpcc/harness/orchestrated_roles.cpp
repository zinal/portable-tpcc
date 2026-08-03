#include "orchestrated_roles.h"

#include <log.h>
#include <time_util.h>
#include <warehouse_range.h>

#include <stdexcept>
#include <unistd.h>

namespace NTpcc {

namespace {

struct TDocHistogramParams {
    uint64_t HdrTill = 4096;
    uint64_t MaxValue = 32768;
    bool RecordUs = false;
};

TDocHistogramParams ResolveDocHistogram(const THistogramConfig& histogram) {
    TDocHistogramParams p;
    p.RecordUs = histogram.Configured && histogram.Unit == "us";
    p.HdrTill = histogram.Configured ? histogram.HdrTill() : 4096ull;
    p.MaxValue = histogram.Configured ? histogram.MaxValue() : 32768ull;
    return p;
}

} // anonymous

int RunOrchestratedLoader(
    const TRunConfigDocument& doc,
    const std::string& instance,
    const TAdapterIdentity& id,
    const TLoaderRoleHooks& hooks)
{
    const auto assign = FindLoaderAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "loader", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "loader", static_cast<int>(::getpid()), nonce);

    const auto clockCalibration = hooks.Calibrate(doc);
    WriteReadyJson(
        paths, doc, instance, assign.WarehouseRanges, nonce, clockCalibration, id.AdapterName);

    int exitCode = 0;
    try {
        hooks.Import(doc, assign);
    } catch (const std::exception& ex) {
        LOG_E("Loader failed: " << ex.what());
        exitCode = 1;
    }

    WriteLoaderResultJson(paths, doc, instance, assign, exitCode);
    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

int RunOrchestratedWorker(
    const TRunConfigDocument& doc,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const TAdapterIdentity& id,
    const TWorkerRoleHooks& hooks)
{
    const auto assign = FindWorkerAssignment(doc, instance);
    const std::string instanceDir = InstanceWorkDir(doc, "worker", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "worker", static_cast<int>(::getpid()), nonce);

    const auto clockCalibration = hooks.Calibrate(doc);
    WriteReadyJson(
        paths, doc, instance, assign.WarehouseRanges, nonce, clockCalibration, id.AdapterName);
    if (!IsClockSkewWithinBudget(clockCalibration, doc.PhasePolicy.MaxClockSkewMs)) {
        LOG_E(instance + ": " + FormatClockSkewViolation(clockCalibration, doc.PhasePolicy.MaxClockSkewMs));
        const auto hp = ResolveDocHistogram(doc.Histogram);
        TTerminalStats emptyStats(hp.HdrTill, hp.MaxValue, hp.RecordUs);
        const auto now = std::chrono::system_clock::now();
        WriteWorkerResultJson(
            paths, doc, instance, assign, emptyStats, false,
            now, now, now, now, 0.0, 1, nonce, id.AdapterName, id.DefaultBinary);
        WriteArtifactManifest(paths, instance, nonce, 1);
        return 1;
    }

    if (!startAtRfc3339.has_value()) {
        throw std::runtime_error("orchestrated worker requires --start-at=<RFC3339-UTC>");
    }
    const auto startAt = ParseRfc3339Utc(*startAtRfc3339);

    const auto hp = ResolveDocHistogram(doc.Histogram);
    TTerminalStats aggregated(hp.HdrTill, hp.MaxValue, hp.RecordUs);
    TRunOutcome outcome;
    int exitCode = 0;
    try {
        outcome = hooks.Run(doc, assign, instanceDir, startAt, aggregated);
        exitCode = outcome.ExitCode;
    } catch (const std::exception& ex) {
        LOG_E("Worker failed: " << ex.what());
        exitCode = 1;
    }

    WriteWorkerResultJson(
        paths, doc, instance, assign, aggregated, outcome.HighResHistogram,
        outcome.RampStart, outcome.MeasurementStart, outcome.MeasurementEnd,
        outcome.DrainDeadline, outcome.MeasurementSeconds, exitCode, nonce,
        id.AdapterName, id.DefaultBinary);
    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

int RunOrchestratedSchema(
    const TRunConfigDocument& doc,
    const std::string& instance,
    std::function<void(const TRunConfigDocument&)> ensureSchema)
{
    const std::string instanceDir = InstanceWorkDir(doc, "schema", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();

    WriteProcessJson(paths, doc, instance, "schema", static_cast<int>(::getpid()), nonce);

    int exitCode = 0;
    try {
        ensureSchema(doc);
    } catch (const std::exception& ex) {
        LOG_E("Schema failed: " << ex.what());
        exitCode = 1;
    }

    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

} // namespace NTpcc
