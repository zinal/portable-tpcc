#pragma once

#include "artifacts.h"
#include "clock_skew.h"
#include "run_config_document.h"
#include "run_loop.h"
#include "terminal.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>

namespace NTpcc {

struct TAdapterIdentity {
    std::string AdapterName;
    std::string DefaultBinary;
};

struct TLoaderRoleHooks {
    // CheckDb + MeasureClockCalibration (opens its own connection).
    std::function<TClockCalibration(const TRunConfigDocument&)> Calibrate;
    // CheckDb (if not done in Calibrate) + fill import config + ImportSync; throw on failure.
    std::function<void(const TRunConfigDocument&, const TLoaderAssignment&)> Import;
};

struct TWorkerRoleHooks {
    // CheckDb + MeasureClockCalibration.
    std::function<TClockCalibration(const TRunConfigDocument&)> Calibrate;
    // Fill adapter TRunConfig + RunSync; return outcome (throws on hard failure).
    std::function<TRunOutcome(
        const TRunConfigDocument& doc,
        const TWorkerAssignment& assign,
        const std::string& instanceDir,
        std::chrono::system_clock::time_point startAt,
        TTerminalStats& aggregated)> Run;
};

int RunOrchestratedLoader(
    const TRunConfigDocument& doc,
    const std::string& instance,
    const TAdapterIdentity& id,
    const TLoaderRoleHooks& hooks);

int RunOrchestratedWorker(
    const TRunConfigDocument& doc,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const TAdapterIdentity& id,
    const TWorkerRoleHooks& hooks);

int RunOrchestratedSchema(
    const TRunConfigDocument& doc,
    const std::string& instance,
    std::function<void(const TRunConfigDocument&)> ensureSchema);

} // namespace NTpcc
