#pragma once

#include <phase_controller.h>
#include <phase_policy.h>
#include <terminal.h>
#include <warehouse_range.h>
#include <workload_config.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

namespace NTpcc {

class ITaskQueue;

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

// Worker progress / console-stats line. Profile runtime.stats_interval,
// run-config runtime.stats_interval_ms, standalone --stats-interval.
inline constexpr int kDefaultStatsIntervalSeconds = 30;
inline constexpr auto kDefaultStatsInterval = std::chrono::seconds(kDefaultStatsIntervalSeconds);
inline constexpr int64_t kDefaultStatsIntervalMs =
    std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultStatsInterval).count();

struct TRunStatsConfig {
    size_t WarehouseCount = 0;
    size_t ThreadCount = 0;
    size_t MaxInflight = 0;
    bool NoDelays = false;
    std::chrono::seconds RunDuration{600};
    THistogramConfig Histogram;
    bool HighResHistogram = false;
    std::chrono::milliseconds StatsInterval{kDefaultStatsInterval};
};

// Progress ticks every StatsInterval (default 30s). Three glued samples of
// Inflight staying near ThreadCount while max_inflight is larger and the
// ready queue is backlogged — the pre-async scheduler-blocking regression.
inline constexpr size_t kInflightStuckMinConsecutiveSamples = 3;
inline constexpr size_t kInflightStuckMinReadyBacklog = 16;

inline std::chrono::milliseconds StatsIntervalFromMs(int64_t ms) {
    if (ms <= 0) {
        return kDefaultStatsInterval;
    }
    return std::chrono::milliseconds(ms);
}

inline bool ShouldUpdateConsoleStats(
    Clock::time_point lastUpdate,
    Clock::time_point now,
    std::chrono::milliseconds interval)
{
    if (lastUpdate.time_since_epoch().count() == 0) {
        return true;
    }
    if (interval <= std::chrono::milliseconds::zero()) {
        interval = kDefaultStatsInterval;
    }
    return now - lastUpdate >= interval;
}

struct TInflightStuckState {
    size_t ConsecutiveGlued = 0;
    bool Warned = false;
};

// Returns true once, when the warning should be logged.
bool ObserveSchedulerInflightStuck(
    TInflightStuckState& state,
    size_t inflight,
    size_t ready,
    size_t threadCount,
    size_t maxInflight);

struct TLatencyConstraintViolation {
    const char* TypeName = "";
    uint64_t P90Ms = 0;
    uint64_t LimitMs = 0;
};

// Histogram bucket values are milliseconds unless unit is "us".
uint64_t PercentileToMilliseconds(uint64_t value, const char* unit);

// TPC-C 5.11 Clause 5.2.5.3 / 5.2.5.7: p90 must be strictly less than the limit.
void CollectLatencyConstraintViolations(
    const TTerminalStats& aggregated,
    const char* unit,
    std::vector<TLatencyConstraintViolation>& out);

std::string FormatInvalidRunLatencyBanner(
    const std::vector<TLatencyConstraintViolation>& violations);

struct TProgressDisplayState {
    Clock::time_point LastUpdate{};
    TInflightStuckState InflightStuck;
    // ProgressOK+ProgressUserAborted at the previous printed line, per type.
    // Cleared when live counters reset at measurement start.
    std::array<size_t, TRANSACTION_TYPE_COUNT> LastProgressCompleted{};
};

// Per-type fragment of the progress line. Counts are increments since
// `lastCompleted` (updated to the current cumulative). p90 is the phase
// response time: measurement LatencyHistogramFullMs once it has samples,
// otherwise the warmup-only ProgressLatencyFullMs. No p50/p99.
std::string FormatProgressTransactionFields(
    const TTerminalStats& aggregated,
    const std::array<size_t, TRANSACTION_TYPE_COUNT>& cumulativeCompleted,
    std::array<size_t, TRANSACTION_TYPE_COUNT>& lastCompleted,
    const char* unit);

// Throttled progress line: phase name, remaining/total seconds left in the
// phase, and live tpmC from Progress* counters (including ramp).
// Per-type counts are the increment since the previous line; each seen type
// shows the phase p90. When taskQueue is set, also append scheduler ready
// depth and sleep overshoot.
void MaybeUpdateConsoleStats(
    TProgressDisplayState& state,
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    ERunPhase phase,
    const TPhaseSchedule& schedule,
    Clock::time_point rampStartSteady,
    Clock::time_point measureStartSteady,
    Clock::time_point measureEndSteady,
    ITaskQueue* taskQueue = nullptr);

void PrintFinalResults(
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    std::chrono::duration<double> measureElapsed,
    ITaskQueue* taskQueue = nullptr);

void RunMeasurementDrainLoop(
    TPhaseController& phaseController,
    const TPhaseSchedule& schedule,
    bool asyncDelivery,
    std::stop_token stopToken,
    std::chrono::milliseconds sleepEvery,
    std::function<void(ERunPhase phase)> maybeUpdateDisplay);

} // namespace NTpcc
