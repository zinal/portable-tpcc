#pragma once

#include "phase_policy.h"
#include "warehouse_range.h"

#include "terminal.h"

#include <chrono>
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
    // SELECT 1 loop or pure sleep. Useful for testing the coroutine/IO stack.
    int SimulateTransactionMs = 0;
    int SimulateTransactionSelect1 = 0;
    bool UseTui = false;

    // Orchestrated worker fields (portable-tpcc run-config).
    bool Orchestrated = false;
    std::vector<TWarehouseRange> WarehouseRanges;
    size_t ScaleWarehouses = 0;
    TPhasePolicy PhasePolicy;
    std::string Instance;
    std::string InstanceDir;

    // Total attempts per business transaction (including the first). 0 → default 4.
    size_t RetryMaxAttempts = 0;
    // When false (default), AmbiguousCommit MUST NOT be blind-retried.
    bool RetryAmbiguousCommit = false;

    bool IsSimulationMode() const {
        return SimulateTransactionMs > 0 || SimulateTransactionSelect1 > 0;
    }

    static constexpr auto SleepMsEveryIterationMainLoop = std::chrono::milliseconds(50);
};

struct TRunOutcome {
    std::chrono::system_clock::time_point RampStart;
    std::chrono::system_clock::time_point MeasurementStart;
    std::chrono::system_clock::time_point MeasurementEnd;
    double MeasurementSeconds = 0.0;
    int ExitCode = 0;
    bool HighResHistogram = false;
};

TRunOutcome RunSync(const TRunConfig& config, TTerminalStats* aggregatedStats = nullptr);

} // namespace NTpcc
