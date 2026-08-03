#include "runner.h"

#include "warehouse_range.h"

#include <constants.h>
#include <log.h>
#include <log_backend.h>
#include "pg_connection_pool.h"
#include "pg_capabilities.h"
#include "runner_display_data.h"
#include <phase_controller.h>
#include <task_queue.h>
#include "terminal.h"
#include "tpcc_session.h"
#include "transactions.h"
#include <domain_util.h>
#include <time_util.h>

#ifdef TPCC_HAS_TUI
#include "runner_tui.h"
#endif

#include <fmt/format.h>

#include <chrono>
#include <csignal>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace NTpcc {

namespace {

void InterruptHandler(int) {
    GetGlobalInterruptSource().request_stop();
}

const char* TransactionTypeName(ETransactionType type) {
    switch (type) {
        case ETransactionType::NewOrder: return "NewOrder";
        case ETransactionType::Delivery: return "Delivery";
        case ETransactionType::OrderStatus: return "OrderStatus";
        case ETransactionType::Payment: return "Payment";
        case ETransactionType::StockLevel: return "StockLevel";
        default: return "Unknown";
    }
}

#ifdef TPCC_HAS_TUI
std::shared_ptr<TRunDisplayData> CollectDisplayData(
    const TRunConfig& config,
    size_t threadCount,
    size_t terminalCount,
    ITaskQueue& taskQueue,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    Clock::time_point startTs,
    Clock::time_point warmupEnd,
    Clock::time_point runEnd,
    bool warmupDone)
{
    auto now = Clock::now();
    auto data = std::make_shared<TRunDisplayData>(threadCount, now, config.HighResHistogram);
    data->WarehouseCount = config.WarehouseCount;

    for (size_t i = 0; i < threadCount; ++i) {
        taskQueue.CollectStats(i, *data->Statistics.StatVec[i].TaskThreadStats);
        perThreadStats[i]->Collect(*data->Statistics.StatVec[i].TerminalStats);
    }

    auto& status = data->StatusData;
    auto totalElapsed = std::chrono::duration<double>(now - startTs);
    auto remaining = std::chrono::duration<double>(runEnd - now);
    auto totalDuration = std::chrono::duration<double>(runEnd - startTs);

    int elapsedSec = static_cast<int>(totalElapsed.count());
    int remainSec = std::max(0, static_cast<int>(remaining.count()));

    status.ElapsedMinutesTotal = elapsedSec / 60;
    status.ElapsedSecondsTotal = elapsedSec % 60;
    status.RemainingMinutesTotal = remainSec / 60;
    status.RemainingSecondsTotal = remainSec % 60;

    double totalSec = totalDuration.count();
    status.ProgressPercentTotal = totalSec > 0 ? std::min(100.0, totalElapsed.count() / totalSec * 100.0) : 100.0;

    status.Phase = warmupDone ? "Measuring" : "Warmup";
    status.RunningTerminals = terminalCount;
    status.RunningTransactions = TransactionsInflight.load(std::memory_order_relaxed);

    size_t totalNewOrderCompleted = 0;
    for (auto& stats : perThreadStats) {
        const auto& no = stats->GetStats(ETransactionType::NewOrder);
        totalNewOrderCompleted += no.OK.load(std::memory_order_relaxed)
            + no.UserAborted.load(std::memory_order_relaxed);
    }

    double measureSeconds = warmupDone ? std::chrono::duration<double>(now - warmupEnd).count() : 0.0;
    status.Tpmc = measureSeconds > 0 ? (totalNewOrderCompleted / measureSeconds * 60.0) : 0.0;
    status.Efficiency = config.WarehouseCount > 0
        ? (status.Tpmc / (MAX_TPMC_PER_WAREHOUSE * config.WarehouseCount) * 100.0) : 0.0;

    return data;
}
#endif

void PrintConsoleStats(
    const TRunConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    Clock::time_point measureStart,
    Clock::time_point runEnd)
{
    auto now = Clock::now();
    auto elapsed = std::chrono::duration<double>(now - measureStart).count();
    auto remaining = std::chrono::duration<double>(runEnd - now).count();

    size_t totalOK = 0;
    size_t totalFailed = 0;
    size_t totalNewOrderCompleted = 0;
    const uint64_t aggHdr = config.Histogram.Configured
        ? config.Histogram.HdrTill()
        : (config.HighResHistogram ? 16384ull : 4096ull);
    const uint64_t aggMax = config.Histogram.Configured
        ? config.Histogram.MaxValue()
        : 32768ull;
    const bool aggUs = config.Histogram.Configured && config.Histogram.Unit == "us";
    TTerminalStats aggregated(aggHdr, aggMax, aggUs);

    for (auto& stats : perThreadStats) {
        stats->Collect(aggregated);
        for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
            totalOK += stats->GetStats(static_cast<ETransactionType>(i)).OK.load(std::memory_order_relaxed);
            totalFailed += stats->GetStats(static_cast<ETransactionType>(i)).Failed.load(std::memory_order_relaxed);
        }
        const auto& no = stats->GetStats(ETransactionType::NewOrder);
        totalNewOrderCompleted += no.OK.load(std::memory_order_relaxed)
            + no.UserAborted.load(std::memory_order_relaxed);
    }

    double tpmc = elapsed > 0 ? (totalNewOrderCompleted / elapsed * 60.0) : 0.0;
    double efficiency = config.WarehouseCount > 0
        ? (tpmc / (MAX_TPMC_PER_WAREHOUSE * config.WarehouseCount) * 100.0) : 0.0;

    std::string latencies;
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        auto p50 = s.LatencyHistogramFullMs.GetValueAtPercentile(50);
        auto p99 = s.LatencyHistogramFullMs.GetValueAtPercentile(99);
        auto completed = s.OK.load(std::memory_order_relaxed)
            + s.UserAborted.load(std::memory_order_relaxed);
        if (completed > 0) {
            latencies += fmt::format("  {}:{}(p50={} p99={})",
                TransactionTypeName(type), completed, p50, p99);
        }
    }

    if (config.NoDelays) {
        LOG_I(fmt::format("{:.0f}s/{:.0f}s | tpmC:{:.0f} | OK:{} Fail:{} Inflight:{} |{}",
              elapsed, elapsed + remaining, tpmc,
              totalOK, totalFailed,
              TransactionsInflight.load(std::memory_order_relaxed),
              latencies));
    } else {
        LOG_I(fmt::format("{:.0f}s/{:.0f}s | tpmC:{:.0f} eff:{:.1f}% | OK:{} Fail:{} Inflight:{} |{}",
              elapsed, elapsed + remaining, tpmc, efficiency,
              totalOK, totalFailed,
              TransactionsInflight.load(std::memory_order_relaxed),
              latencies));
    }
}

void PrintFinalResults(
    const TRunConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    std::chrono::duration<double> measureElapsed)
{
    const uint64_t aggHdr = config.Histogram.Configured
        ? config.Histogram.HdrTill()
        : (config.HighResHistogram ? 16384ull : 4096ull);
    const uint64_t aggMax = config.Histogram.Configured
        ? config.Histogram.MaxValue()
        : 32768ull;
    const bool aggUs = config.Histogram.Configured && config.Histogram.Unit == "us";
    TTerminalStats aggregated(aggHdr, aggMax, aggUs);
    size_t totalFailed = 0;

    for (auto& stats : perThreadStats) {
        stats->Collect(aggregated);
        for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
            totalFailed += stats->GetStats(static_cast<ETransactionType>(i)).Failed.load(std::memory_order_relaxed);
        }
    }

    const auto& newOrderStats = aggregated.GetStats(ETransactionType::NewOrder);
    size_t totalNewOrderCompleted = newOrderStats.OK.load(std::memory_order_relaxed)
        + newOrderStats.UserAborted.load(std::memory_order_relaxed);
    double measureDuration = measureElapsed.count();
    double tpmc = measureDuration > 0 ? (totalNewOrderCompleted / measureDuration * 60.0) : 0.0;
    double efficiency = config.WarehouseCount > 0
        ? (tpmc / (MAX_TPMC_PER_WAREHOUSE * config.WarehouseCount) * 100.0) : 0.0;

    LOG_I("=== TPC-C Results ===");
    LOG_I(fmt::format("  Measured Duration: {:.1f}s (configured: {}s)",
          measureDuration, config.RunDuration.count()));
    LOG_I(fmt::format("  New-Order Throughput: {:.2f} tpmC", tpmc));
    if (!config.NoDelays) {
        LOG_I(fmt::format("  Efficiency: {:.1f}%", efficiency));
    }
    LOG_I("  Total Failed: " << totalFailed);

    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        auto ok = s.OK.load(std::memory_order_relaxed);
        auto failed = s.Failed.load(std::memory_order_relaxed);
        auto userAborted = s.UserAborted.load(std::memory_order_relaxed);
        if (ok == 0 && failed == 0 && userAborted == 0) continue;

        LOG_I("  " << TransactionTypeName(type) << ": OK=" << ok << " UserAborted=" << userAborted << " Failed=" << failed << " p50=" << s.LatencyHistogramFullMs.GetValueAtPercentile(50) << "ms p90=" << s.LatencyHistogramFullMs.GetValueAtPercentile(90) << "ms p99=" << s.LatencyHistogramFullMs.GetValueAtPercentile(99) << "ms");
    }
}

} // anonymous

TRunOutcome RunSync(const TRunConfig& config, TTerminalStats* aggregatedStats) {
    TRunOutcome outcome;
    signal(SIGINT, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    std::vector<TWarehouseRange> ranges = config.WarehouseRanges;
    if (ranges.empty()) {
        ranges.push_back(TWarehouseRange{1, static_cast<int>(config.WarehouseCount) + 1});
    }
    const size_t warehouseCount = CountWarehouses(ranges);
    const size_t scaleWarehouses = config.ScaleWarehouses > 0 ? config.ScaleWarehouses : warehouseCount;
    const size_t terminalsPerWh = config.Workload.TerminalsPerWarehouse > 0
        ? config.Workload.TerminalsPerWarehouse
        : TERMINALS_PER_WAREHOUSE;
    const size_t terminalCount = warehouseCount * terminalsPerWh;

    const size_t maxInflight = config.MaxInflight;
    if (maxInflight == 0) {
        throw std::runtime_error("MaxInflight must be greater than zero");
    }
    const size_t poolSize = std::min(terminalCount, maxInflight);

    // Resolve ioThreads early so we can reserve CPU for them when sizing the
    // terminal thread pool below.
    size_t ioThreads = config.IOThreads;
    if (ioThreads == 0) {
        ioThreads = maxInflight;
    }
    ioThreads = std::max(ioThreads, poolSize);

    const size_t cpuCount = NumberOfMyCpus();
    const size_t reservedForIo = std::min(ioThreads, std::max<size_t>(cpuCount / 4, 1));
    const size_t maxTerminalThreadCountAvailable =
        cpuCount > reservedForIo ? cpuCount - reservedForIo : 1;

    const size_t recommendedThreadCount =
        (warehouseCount + WAREHOUSES_PER_CPU_CORE - 1) / WAREHOUSES_PER_CPU_CORE;

    size_t threadCount;
    if (config.ThreadCount == 0) {
        threadCount = std::min(maxTerminalThreadCountAvailable, terminalCount);
        threadCount = std::min(threadCount, recommendedThreadCount);

        // Even count looks nicer in the TUI, if we still have headroom.
        if (threadCount % 2 != 0 && threadCount < maxTerminalThreadCountAvailable) {
            ++threadCount;
        }
    } else {
        threadCount = config.ThreadCount;
        if (threadCount > maxTerminalThreadCountAvailable) {
            LOG_I("User provided thread count " << threadCount << " is above max available " << maxTerminalThreadCountAvailable << " "
                  "(cpu count " << cpuCount << ", io threads " << ioThreads << "). Recommended for " << warehouseCount << " warehouses is " << recommendedThreadCount << ". "
                  "Setting thread count to " << maxTerminalThreadCountAvailable);
            threadCount = maxTerminalThreadCountAvailable;
        }
    }
    threadCount = std::max(threadCount, size_t(1));

    if (threadCount < recommendedThreadCount) {
        LOG_W("Thread count " << threadCount << " is lower than recommended " << recommendedThreadCount << ". "
              "It might affect benchmark results");
    }

    if (config.IsSimulationMode()) {
        LOG_I("SIMULATION MODE: " << config.SimulateTransactionSelect1 << " SELECT 1 queries per transaction");
    }

    LOG_I("Starting TPC-C benchmark: " << warehouseCount << " warehouses, " << terminalCount
          << " terminals, " << threadCount << " threads, "
          << poolSize << " connections, "
          << maxInflight << " max inflight");

    auto connectionPool = std::make_unique<PgConnectionPool>(
        config.ConnectionString, poolSize, ioThreads, config.Path);
    auto sessionFactory = std::make_unique<TPgSessionFactory>(*connectionPool);

    auto taskQueue = CreateTaskQueue(threadCount, maxInflight, terminalCount, terminalCount);

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
    perThreadStats.reserve(threadCount);
    for (size_t i = 0; i < threadCount; ++i) {
        perThreadStats.push_back(std::make_shared<TTerminalStats>(histHdr, histMax, recordUs));
    }

    std::vector<std::unique_ptr<TTerminal>> terminals;
    terminals.reserve(terminalCount);

    size_t terminalIndex = 0;
    for (const auto& range : ranges) {
        for (int wh = range.Start; wh < range.End; ++wh) {
            for (size_t t = 0; t < terminalsPerWh; ++t) {
                const size_t threadIndex = terminalIndex % threadCount;
                const size_t districtID = static_cast<size_t>(HomeDistrictId(t));

                terminals.push_back(std::make_unique<TTerminal>(
                    terminalIndex,
                    static_cast<size_t>(wh),
                    districtID,
                    scaleWarehouses,
                    *taskQueue,
                    sessionFactory.get(),
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

    constexpr auto MinWarmupPerTerminalMs = std::chrono::milliseconds(1);
    uint32_t minWarmupSeconds =
        static_cast<uint32_t>(terminalCount * MinWarmupPerTerminalMs.count() / 1000 + 1);

    bool forcedWarmup = false;
    TPhaseDurations durations;

    if (config.Orchestrated || config.StartAt.has_value()) {
        durations.RampUpMs = config.PhasePolicy.RampUpMs;
        durations.MeasurementMs = config.PhasePolicy.MeasurementMs;
        durations.TransactionDrainMs = config.PhasePolicy.TransactionDrainMs;
        durations.StopGraceMs = config.PhasePolicy.StopGraceMs;
        if (durations.MeasurementMs <= 0 && !config.Orchestrated) {
            durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                config.RunDuration).count();
        }
    } else if (config.SkipWarmup) {
        durations.RampUpMs = 0;
        durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            config.RunDuration).count();
    } else {
        uint32_t warmupSeconds = 0;
        if (config.WarmupDuration.count() == 0) {
            if (warehouseCount <= 10) {
                warmupSeconds = 30;
            } else if (warehouseCount <= 100) {
                warmupSeconds = 5 * 60;
            } else if (warehouseCount <= 1000) {
                warmupSeconds = 10 * 60;
            } else {
                warmupSeconds = 30 * 60;
            }
            warmupSeconds = std::max(warmupSeconds, minWarmupSeconds);
        } else {
            warmupSeconds = static_cast<uint32_t>(config.WarmupDuration.count());
            if (warmupSeconds < minWarmupSeconds) {
                forcedWarmup = true;
                warmupSeconds = minWarmupSeconds;
            }
        }
        durations.RampUpMs = static_cast<int64_t>(warmupSeconds) * 1000;
        durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            config.RunDuration).count();
    }

    // async_delivery=false (PG): drain waits for in-flight only; no async queue.
    const bool asyncDelivery = TPgCapabilities{}.Get().AsyncDelivery;
    if (!asyncDelivery) {
        // Keep TransactionDrainMs as the max wait for in-flight to finish.
    }

    // Start terminals during prepare (they wait until MayAdmit).
    for (size_t i = 0; i < terminals.size() && !stopToken.stop_requested(); ++i) {
        terminals[i]->Start();
        std::this_thread::sleep_for(MinWarmupPerTerminalMs);
    }

    using SysClock = std::chrono::system_clock;
    const auto preparedAt = SysClock::now();

    SysClock::time_point rampStart;
    if (config.StartAt.has_value()) {
        rampStart = *config.StartAt;
        if (preparedAt >= rampStart) {
            LOG_E("Missed --start-at deadline " << FormatRfc3339Utc(rampStart) << ": prepare finished at " << FormatRfc3339Utc(preparedAt));
            GetGlobalErrorVariable().store(true);
            GetGlobalInterruptSource().request_stop();
            connectionPool->CancelAll();
            taskQueue->WakeupAndNeverSleep();
            taskQueue->Join();
            outcome.ExitCode = 1;
            return outcome;
        }
        LOG_I("Prepared; waiting until start-at " << FormatRfc3339Utc(rampStart) << " (" << std::chrono::duration_cast<std::chrono::milliseconds>(rampStart - preparedAt).count() << " ms)");
        while (!stopToken.stop_requested()) {
            const auto now = SysClock::now();
            if (now >= rampStart) {
                break;
            }
            const auto remain = rampStart - now;
            const auto slice = remain > std::chrono::milliseconds(50)
                ? std::chrono::milliseconds(50)
                : std::chrono::duration_cast<std::chrono::milliseconds>(remain);
            std::this_thread::sleep_for(slice);
        }
        if (stopToken.stop_requested()) {
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

    if (forcedWarmup) {
        LOG_I("Forced minimal warmup: " << durations.RampUpMs << "ms");
    }

    LOG_I("Phase schedule: ramp=" << FormatRfc3339Utc(schedule.RampStart) << " measure_start=" << FormatRfc3339Utc(schedule.MeasurementStart) << " measure_end=" << FormatRfc3339Utc(schedule.MeasurementEnd) << " drain=" << FormatRfc3339Utc(schedule.DrainDeadline));

    phaseController.SetPhase(ERunPhase::Ramp);
    if (durations.RampUpMs <= 0) {
        phaseController.SetPhase(ERunPhase::Measure);
    }

    auto startTs = Clock::now();
    auto warmupEnd = startTs + std::chrono::milliseconds(durations.RampUpMs);
    auto runEnd = warmupEnd + std::chrono::milliseconds(durations.MeasurementMs);

#ifdef TPCC_HAS_TUI
    std::unique_ptr<TRunnerTui> tui;
    if (config.UseTui) {
        StartLogCapture();
        auto initData = CollectDisplayData(
            config, threadCount, terminalCount, *taskQueue,
            perThreadStats, startTs, warmupEnd, runEnd, durations.RampUpMs <= 0);
        tui = std::make_unique<TRunnerTui>(*GetLogBackend(), initData);
    }
#endif

    Clock::time_point lastDisplayUpdate = startTs;
    std::shared_ptr<TRunDisplayData> prevData;

    auto maybeUpdateDisplay = [&](Clock::time_point now, bool warmupDone) {
        auto sinceLast = std::chrono::duration_cast<std::chrono::seconds>(now - lastDisplayUpdate);
        const auto updateInterval = std::chrono::seconds(
#ifdef TPCC_HAS_TUI
            tui ? 1 :
#endif
            5);
        if (sinceLast < updateInterval) {
            return;
        }
#ifdef TPCC_HAS_TUI
        if (tui) {
            auto displayData = CollectDisplayData(
                config, threadCount, terminalCount, *taskQueue,
                perThreadStats, startTs, warmupEnd, runEnd, warmupDone);
            if (prevData) {
                displayData->Statistics.CalculateDerivativeAndTotal(prevData->Statistics);
            }
            tui->Update(displayData);
            prevData = displayData;
        } else
#endif
        {
            auto measureStart = warmupDone ? warmupEnd : startTs;
            PrintConsoleStats(config, perThreadStats, measureStart, runEnd);
        }
        lastDisplayUpdate = now;
    };

    while (!stopToken.stop_requested()) {
        const auto wallNow = SysClock::now();
        phaseController.Tick(wallNow);
        const auto phase = phaseController.Phase();

        if (phase == ERunPhase::Drain || phase == ERunPhase::Stop) {
            // Stop admission; wait for in-flight (async_delivery=false → no async drain).
            const size_t inflight = TransactionsInflight.load(std::memory_order_relaxed);
            if (inflight == 0) {
                LOG_I("Drain complete (in-flight=0)");
                break;
            }
            if (wallNow >= schedule.DrainDeadline) {
                LOG_I("Drain deadline reached with " << inflight << " in-flight transactions");
                break;
            }
            if (!asyncDelivery) {
                LOG_T("Draining in-flight=" << inflight);
            }
        }

        maybeUpdateDisplay(Clock::now(), phase == ERunPhase::Measure || phase == ERunPhase::Drain);
        std::this_thread::sleep_for(TRunConfig::SleepMsEveryIterationMainLoop);
    }

    phaseController.SetPhase(ERunPhase::Stop);

#ifdef TPCC_HAS_TUI
    tui.reset();
    StopLogCapture();
#endif

    auto measureElapsed = std::chrono::duration<double>(
        schedule.MeasurementEnd - schedule.MeasurementStart);

    LOG_I("Stopping terminals...");
    GetGlobalInterruptSource().request_stop();
    connectionPool->CancelAll();
    taskQueue->WakeupAndNeverSleep();
    taskQueue->Join();

    PrintFinalResults(config, perThreadStats, measureElapsed);

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
