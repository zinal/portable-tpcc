#pragma once

#include <phase_controller.h>
#include <phase_policy.h>
#include <terminal.h>
#include <warehouse_range.h>
#include <workload_config.h>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace NTpcc {

struct TRunOutcome {
    std::chrono::system_clock::time_point RampStart;
    std::chrono::system_clock::time_point MeasurementStart;
    std::chrono::system_clock::time_point MeasurementEnd;
    std::chrono::system_clock::time_point DrainDeadline;
    double MeasurementSeconds = 0.0;
    int ExitCode = 0;
    bool HighResHistogram = false;
};

inline constexpr auto kRunLoopSleepEvery = std::chrono::milliseconds(50);

struct TRunSizingInput {
    std::vector<TWarehouseRange> WarehouseRanges;
    size_t WarehouseCount = 0;
    size_t ScaleWarehouses = 0;
    TWorkloadConfig Workload = MakeDefaultWorkloadConfig();
    size_t MaxInflight = 0;
    size_t IOThreads = 0;
    size_t ThreadCount = 0;
};

struct TRunLayout {
    std::vector<TWarehouseRange> Ranges;
    size_t WarehouseCount = 0;
    size_t ScaleWarehouses = 0;
    size_t TerminalsPerWarehouse = 0;
    size_t TerminalCount = 0;
    size_t MaxInflight = 0;
    size_t PoolSize = 0;
    size_t IoThreads = 0;
    size_t ThreadCount = 0;
    size_t RecommendedThreadCount = 0;
};

TRunLayout ComputeRunLayout(const TRunSizingInput& input);

struct TPhaseDurationInput {
    bool Orchestrated = false;
    bool HasStartAt = false;
    bool SkipWarmup = false;
    TPhasePolicy PhasePolicy;
    std::chrono::seconds RunDuration{600};
    std::chrono::seconds WarmupDuration{0};
    size_t WarehouseCount = 0;
    size_t TerminalCount = 0;
};

struct TPhaseDurationResult {
    TPhaseDurations Durations;
    bool ForcedWarmup = false;
};

TPhaseDurationResult ResolvePhaseDurations(const TPhaseDurationInput& input);

enum class EStartAtWaitResult {
    Ok,
    MissedDeadline,
    Interrupted,
};

// Logs wait / missed-deadline messages. On MissedDeadline, sets the global error
// flag and requests stop. Caller cancels pools / joins task queues on failure.
EStartAtWaitResult WaitUntilStartAt(
    std::chrono::system_clock::time_point rampStart,
    std::chrono::system_clock::time_point preparedAt,
    std::stop_token stopToken);

struct TRunStatsConfig {
    size_t WarehouseCount = 0;
    bool NoDelays = false;
    std::chrono::seconds RunDuration{600};
    THistogramConfig Histogram;
    bool HighResHistogram = false;
};

struct TProgressDisplayState {
    Clock::time_point LastUpdate{};
};

// Throttled progress line: phase name, elapsed/total for the phase, seconds
// left until phase end, and live tpmC from Progress* counters (including ramp).
void MaybeUpdateConsoleStats(
    TProgressDisplayState& state,
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    ERunPhase phase,
    const TPhaseSchedule& schedule,
    Clock::time_point rampStartSteady,
    Clock::time_point measureStartSteady,
    Clock::time_point measureEndSteady);

void PrintFinalResults(
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    std::chrono::duration<double> measureElapsed);

void RunMeasurementDrainLoop(
    TPhaseController& phaseController,
    const TPhaseSchedule& schedule,
    bool asyncDelivery,
    std::stop_token stopToken,
    std::chrono::milliseconds sleepEvery,
    std::function<void(ERunPhase phase)> maybeUpdateDisplay);

} // namespace NTpcc
