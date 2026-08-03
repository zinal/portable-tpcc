#pragma once

#include "phase_policy.h"
#include "warehouse_range.h"
#include "terminal.h"

#include <workload_config.h>

#include <phase_controller.h>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace NTpcc {

constexpr int DEFAULT_WAREHOUSE_COUNT = 10;
constexpr int DEFAULT_THREAD_COUNT = 0;
constexpr int DEFAULT_MAX_INFLIGHT = 100;
constexpr int DEFAULT_IO_THREADS = 0;

struct TRunConfig {
    std::string ConnectionString;
    std::string Path;
    size_t WarehouseCount = DEFAULT_WAREHOUSE_COUNT;
    std::chrono::seconds WarmupDuration{0};
    std::chrono::seconds RunDuration{600};
    bool SkipWarmup = false;
    size_t ThreadCount = DEFAULT_THREAD_COUNT;
    size_t MaxInflight = DEFAULT_MAX_INFLIGHT;
    size_t IOThreads = DEFAULT_IO_THREADS;
    bool NoDelays = false;
    bool HighResHistogram = false;

    // Simulation mode: replaces real TPC-C transactions with a lightweight
    // SELECT 1 loop. Useful for testing the coroutine/IO stack.
    int SimulateTransactionSelect1 = 0;
    bool UseTui = false;

    // Orchestrated worker fields (portable-tpcc run-config).
    bool Orchestrated = false;
    std::vector<TWarehouseRange> WarehouseRanges;
    size_t ScaleWarehouses = 0;
    TPhasePolicy PhasePolicy;
    std::string Instance;
    std::string InstanceDir;

    // Absolute wall-clock ramp start (RFC3339). When set, schedule is derived from
    // StartAt + PhasePolicy; prepare must finish before StartAt or the run fails.
    std::optional<std::chrono::system_clock::time_point> StartAt;

    TWorkloadConfig Workload = MakeDefaultWorkloadConfig();
    THistogramConfig Histogram;
    EThinkTimeDistribution ThinkTimeDistribution = EThinkTimeDistribution::Exponential;

    // Total attempts per business transaction (including the first). 0 → default 4.
    size_t RetryMaxAttempts = 0;
    int64_t RetryInitialBackoffMs = 10;
    int64_t RetryMaxBackoffMs = 500;
    std::string RetryJitter = "full";
    // When false (default), AmbiguousCommit MUST NOT be blind-retried.
    bool RetryAmbiguousCommit = false;

    bool IsSimulationMode() const {
        return SimulateTransactionSelect1 > 0;
    }

    static constexpr auto SleepMsEveryIterationMainLoop = std::chrono::milliseconds(50);
};

struct TRunOutcome {
    std::chrono::system_clock::time_point RampStart;
    std::chrono::system_clock::time_point MeasurementStart;
    std::chrono::system_clock::time_point MeasurementEnd;
    std::chrono::system_clock::time_point DrainDeadline;
    double MeasurementSeconds = 0.0;
    int ExitCode = 0;
    bool HighResHistogram = false;
};

TRunOutcome RunSync(const TRunConfig& config, TTerminalStats* aggregatedStats = nullptr);

} // namespace NTpcc
