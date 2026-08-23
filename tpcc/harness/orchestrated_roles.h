#pragma once

#include "artifacts.h"
#include "clock_skew.h"
#include "run_config_document.h"
#include "run_loop.h"
#include "terminal.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
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
    const TLoaderRoleHooks& hooks,
    const std::optional<int>& threadOverride = {});

int RunOrchestratedWorker(
    const TRunConfigDocument& doc,
    const std::string& instance,
    const std::optional<std::string>& startAtRfc3339,
    const TAdapterIdentity& id,
    const TWorkerRoleHooks& hooks,
    const std::optional<int>& threadOverride = {});

int RunOrchestratedSchema(
    const TRunConfigDocument& doc,
    const std::string& instance,
    std::function<void(const TRunConfigDocument&)> ensureSchema);

// Post-load preparation: EnsureIndexes + EnsureStatistics (idempotent).
int RunOrchestratedIndexes(
    const TRunConfigDocument& doc,
    const std::string& instance,
    std::function<void(const TRunConfigDocument&)> ensureIndexes);

// Admin helper for mind-tpcc drop (not a workload role).
int RunOrchestratedDrop(
    const TRunConfigDocument& doc,
    const std::string& instance,
    std::function<void(const TRunConfigDocument&)> drop);

inline void RequireNonNegativeThreads(const std::optional<int>& threads) {
    if (threads.has_value() && *threads < 0) {
        throw std::runtime_error("--threads must not be negative");
    }
}

// Launch-time --threads override. Missing keeps assignment.Threads from
// run-config; 0 means auto at the binary (loader ImportSync / worker ComputeRunLayout).
inline void ApplyOrchestratedThreadOverride(size_t& threads, const std::optional<int>& override) {
    RequireNonNegativeThreads(override);
    if (override.has_value()) {
        threads = static_cast<size_t>(*override);
    }
}

} // namespace NTpcc
