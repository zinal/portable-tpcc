#include "runner.h"

#include "ob_capabilities.h"
#include "ob_connection_pool.h"
#include "ob_error_classifier.h"
#include "tpcc_session.h"
#include "warehouse_range.h"

#include <constants.h>
#include <domain_util.h>
#include <log.h>
#include <phase_controller.h>
#include <run_loop.h>
#include <task_queue.h>
#include <terminal.h>
#include <time_util.h>

#include <chrono>
#include <csignal>
#include <memory>
#include <thread>
#include <vector>

namespace NTpcc {
namespace {

void InterruptHandler(int) {
    GetGlobalInterruptSource().request_stop();
}

TRunStatsConfig MakeRunStatsConfig(const TRunConfig& config) {
    TRunStatsConfig stats;
    stats.WarehouseCount = config.WarehouseCount;
    stats.NoDelays = config.NoDelays;
    stats.RunDuration = config.RunDuration;
    stats.Histogram = config.Histogram;
    stats.HighResHistogram = config.HighResHistogram;
    return stats;
}

} // namespace

TRunOutcome RunSync(const TRunConfig& config, TTerminalStats* aggregatedStats) {
    TRunOutcome outcome;
    signal(SIGINT, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    TRunSizingInput sizing;
    sizing.WarehouseRanges = config.WarehouseRanges;
    sizing.WarehouseCount = config.WarehouseCount;
    sizing.ScaleWarehouses = config.ScaleWarehouses;
    sizing.Workload = config.Workload;
    sizing.MaxInflight = config.MaxInflight;
    sizing.IOThreads = config.IOThreads;
    sizing.ThreadCount = config.ThreadCount;
    const TRunLayout layout = ComputeRunLayout(sizing);

    if (config.IsSimulationMode()) {
        LOG_I("SIMULATION MODE: " << config.SimulateTransactionSelect1 << " SELECT 1 queries per transaction");
    }

    LOG_I("Starting TPC-C benchmark: " << layout.WarehouseCount << " warehouses, "
          << layout.TerminalCount << " terminals, " << layout.ThreadCount << " threads, "
          << layout.PoolSize << " connections, " << layout.MaxInflight << " max inflight");

    auto connectionPool = std::make_unique<TObConnectionPool>(
        config.ConnectionString, layout.PoolSize, layout.IoThreads, config.Path);
    auto sessionFactory = std::make_unique<TObSessionFactory>(*connectionPool);

    auto taskQueue = CreateTaskQueue(
        layout.ThreadCount, layout.MaxInflight, layout.TerminalCount, layout.TerminalCount);

    auto stopToken = GetGlobalInterruptSource().get_token();
    TPhaseController phaseController;

    const bool recordUs = config.Histogram.Configured && config.Histogram.Unit == "us";
    const uint64_t histHdr = config.Histogram.Configured
        ? config.Histogram.HdrTill()
        : (config.HighResHistogram ? 16384ull : 4096ull);
    const uint64_t histMax = config.Histogram.Configured
        ? config.Histogram.MaxValue()
        : 32768ull;

    std::vector<std::shared_ptr<TTerminalStats>> perThreadStats;
    perThreadStats.reserve(layout.ThreadCount);
    for (size_t i = 0; i < layout.ThreadCount; ++i) {
        perThreadStats.push_back(std::make_shared<TTerminalStats>(histHdr, histMax, recordUs));
    }

    std::vector<std::unique_ptr<TTerminal>> terminals;
    terminals.reserve(layout.TerminalCount);

    TObErrorClassifier errorClassifier;

    size_t terminalIndex = 0;
    for (const auto& range : layout.Ranges) {
        for (int wh = range.Start; wh < range.End; ++wh) {
            for (size_t t = 0; t < layout.TerminalsPerWarehouse; ++t) {
                const size_t threadIndex = terminalIndex % layout.ThreadCount;
                const size_t districtID = static_cast<size_t>(HomeDistrictId(t));

                terminals.push_back(std::make_unique<TTerminal>(
                    terminalIndex,
                    static_cast<size_t>(wh),
                    districtID,
                    layout.ScaleWarehouses,
                    *taskQueue,
                    sessionFactory.get(),
                    &errorClassifier,
                    EIsolationLevel::RepeatableRead,
                    config.NoDelays,
                    stopToken,
                    phaseController,
                    perThreadStats[threadIndex],
                    config.Workload,
                    config.SimulateTransactionSelect1,
                    config.RetryMaxAttempts,
                    config.RetryInitialBackoffMs,
                    config.RetryMaxBackoffMs,
                    config.RetryJitter,
                    config.RetryAmbiguousCommit,
                    config.ThinkTimeDistribution));
                ++terminalIndex;
            }
        }
    }

    taskQueue->Run();

    TPhaseDurationInput phaseInput;
    phaseInput.Orchestrated = config.Orchestrated;
    phaseInput.HasStartAt = config.StartAt.has_value();
    phaseInput.SkipWarmup = config.SkipWarmup;
    phaseInput.PhasePolicy = config.PhasePolicy;
    phaseInput.RunDuration = config.RunDuration;
    phaseInput.WarmupDuration = config.WarmupDuration;
    phaseInput.WarehouseCount = layout.WarehouseCount;
    phaseInput.TerminalCount = layout.TerminalCount;
    const TPhaseDurationResult phaseResult = ResolvePhaseDurations(phaseInput);
    const TPhaseDurations& durations = phaseResult.Durations;

    const bool asyncDelivery = TObCapabilities{config.Partitioning, config.ForeignKeys}.Get().AsyncDelivery;

    constexpr auto MinWarmupPerTerminalMs = std::chrono::milliseconds(1);
    for (size_t i = 0; i < terminals.size() && !stopToken.stop_requested(); ++i) {
        terminals[i]->Start();
        std::this_thread::sleep_for(MinWarmupPerTerminalMs);
    }

    using SysClock = std::chrono::system_clock;
    const auto preparedAt = SysClock::now();

    SysClock::time_point rampStart;
    if (config.StartAt.has_value()) {
        rampStart = *config.StartAt;
        const auto waitResult = WaitUntilStartAt(rampStart, preparedAt, stopToken);
        if (waitResult == EStartAtWaitResult::MissedDeadline) {
            connectionPool->CancelAll();
            taskQueue->WakeupAndNeverSleep();
            taskQueue->Join();
            outcome.ExitCode = 1;
            return outcome;
        }
        if (waitResult == EStartAtWaitResult::Interrupted) {
            phaseController.SetPhase(ERunPhase::Stop);
            GetGlobalInterruptSource().request_stop();
            connectionPool->CancelAll();
            taskQueue->WakeupAndNeverSleep();
            taskQueue->Join();
            outcome.ExitCode = GetGlobalErrorVariable().load() ? 1 : 0;
            return outcome;
        }
    } else {
        rampStart = SysClock::now();
    }

    auto schedule = BuildPhaseSchedule(rampStart, durations);
    phaseController.SetSchedule(schedule);
    outcome.RampStart = schedule.RampStart;
    outcome.MeasurementStart = schedule.MeasurementStart;
    outcome.MeasurementEnd = schedule.MeasurementEnd;
    outcome.DrainDeadline = schedule.DrainDeadline;
    outcome.MeasurementSeconds =
        std::chrono::duration<double>(schedule.MeasurementEnd - schedule.MeasurementStart).count();

    if (phaseResult.ForcedWarmup) {
        LOG_I("Forced minimal warmup: " << durations.RampUpMs << "ms");
    }

    LOG_I("Phase schedule: ramp=" << FormatRfc3339Utc(schedule.RampStart)
          << " measure_start=" << FormatRfc3339Utc(schedule.MeasurementStart)
          << " measure_end=" << FormatRfc3339Utc(schedule.MeasurementEnd)
          << " drain=" << FormatRfc3339Utc(schedule.DrainDeadline));

    phaseController.SetPhase(ERunPhase::Ramp);
    if (durations.RampUpMs <= 0) {
        phaseController.SetPhase(ERunPhase::Measure);
    }

    auto startTs = Clock::now();
    auto warmupEnd = startTs + std::chrono::milliseconds(durations.RampUpMs);
    auto runEnd = warmupEnd + std::chrono::milliseconds(durations.MeasurementMs);

    const TRunStatsConfig statsConfig = MakeRunStatsConfig(config);
    TProgressDisplayState progressState;

    RunMeasurementDrainLoop(
        phaseController,
        schedule,
        asyncDelivery,
        stopToken,
        TRunConfig::SleepMsEveryIterationMainLoop,
        [&](ERunPhase phase) {
            MaybeUpdateConsoleStats(
                progressState,
                statsConfig,
                perThreadStats,
                phase,
                schedule,
                startTs,
                warmupEnd,
                runEnd);
        });

    auto measureElapsed = std::chrono::duration<double>(
        schedule.MeasurementEnd - schedule.MeasurementStart);

    LOG_I("Stopping terminals...");
    GetGlobalInterruptSource().request_stop();
    connectionPool->CancelAll();
    taskQueue->WakeupAndNeverSleep();
    taskQueue->Join();

    PrintFinalResults(statsConfig, perThreadStats, measureElapsed);

    if (aggregatedStats) {
        aggregatedStats->Clear();
        for (auto& stats : perThreadStats) {
            stats->Collect(*aggregatedStats);
        }
    }
    outcome.HighResHistogram = config.HighResHistogram;

    sessionFactory.reset();
    connectionPool.reset();

    outcome.ExitCode = GetGlobalErrorVariable().load() ? 1 : 0;
    return outcome;
}

} // namespace NTpcc
