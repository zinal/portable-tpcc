#include "run_loop.h"

#include <constants.h>
#include <domain_util.h>
#include <log.h>
#include <task_queue.h>
#include <time_util.h>
#include <context.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace NTpcc {

static const char* TransactionTypeName(ETransactionType type) {
    switch (type) {
        case ETransactionType::NewOrder: return "NewOrder";
        case ETransactionType::Delivery: return "Delivery";
        case ETransactionType::OrderStatus: return "OrderStatus";
        case ETransactionType::Payment: return "Payment";
        case ETransactionType::StockLevel: return "StockLevel";
        default: return "Unknown";
    }
}

namespace {

void ResolveHistogramParams(
    const THistogramConfig& histogram,
    bool highResHistogram,
    uint64_t& aggHdr,
    uint64_t& aggMax,
    bool& aggUs)
{
    aggHdr = histogram.Configured
        ? histogram.HdrTill()
        : (highResHistogram ? 16384ull : 4096ull);
    aggMax = histogram.Configured
        ? histogram.MaxValue()
        : 32768ull;
    aggUs = histogram.Configured && histogram.Unit == "us";
}

const char* HistogramUnitLabel(const TRunStatsConfig& config) {
    return (config.Histogram.Configured && config.Histogram.Unit == "us") ? "us" : "ms";
}

// Console progress/final lines always show milliseconds. Histograms may still
// use us buckets internally; convert those bucket edges for display.
std::string FormatPercentile(uint64_t value, const char* unit) {
    if (std::strcmp(unit, "us") == 0) {
        return fmt::format("{:.1f}ms", static_cast<double>(value) / 1000.0);
    }
    return fmt::format("{}ms", value);
}

// `ready` is the sum over scheduler threads of InternalTasksReady.
//
// It is not a count of tasks that became ready during the stats interval.
// Each scheduler turn (TTaskQueue::RunThread in task_queue.cpp) moves due
// sleeps and inflight promotions onto ReadyTasksInternal, resumes up to
// MaxInternalResumesPerIteration (64) of them, and only then stores the
// residual depth. This line samples that residual.
//
// On the async worker path that residual stays 0:
// * DBMS completions are cross-thread. They enter ReadyTasksExternal, which
//   the same turn drains completely, and they are not included in `ready`.
// * co_await TTaskReady on the scheduler thread does not enqueue
//   (await_ready is true when CheckCurrentThread()).
// * The internal queue then only receives same-thread wakeups. Those are
//   pushed and consumed before the counter is published, and a normal turn
//   wakes far fewer than 64 of them.
// A non-zero value means one turn left more than 64 internal tasks queued
// (a timer wave, or the thread was stuck inside resume while timers piled
// up). Left unchanged for now.
std::string FormatSchedulerStats(ITaskQueue* taskQueue) {
    if (!taskQueue) {
        return {};
    }

    ITaskQueue::TThreadStats aggregated;
    uint64_t ready = 0;
    for (size_t i = 0; i < taskQueue->GetThreadCount(); ++i) {
        ITaskQueue::TThreadStats snap;
        taskQueue->CollectStats(i, snap);
        ready += snap.InternalTasksReady.load(std::memory_order_relaxed);
        aggregated.Collect(snap);
    }

    std::string out;
    out += fmt::format(" ready:{}", ready);
    if (aggregated.SleepOvershootMs.TotalCount() > 0) {
        out += fmt::format(" overshoot_p50={}ms overshoot_p99={}ms",
            aggregated.SleepOvershootMs.GetValueAtPercentile(50),
            aggregated.SleepOvershootMs.GetValueAtPercentile(99));
    }
    if (aggregated.InternalQueueTimeMs.TotalCount() > 0) {
        out += fmt::format(" q_p99={}ms",
            aggregated.InternalQueueTimeMs.GetValueAtPercentile(99));
    }
    return out;
}

size_t SchedulerReadyCount(ITaskQueue* taskQueue) {
    if (!taskQueue) {
        return 0;
    }
    uint64_t ready = 0;
    for (size_t i = 0; i < taskQueue->GetThreadCount(); ++i) {
        ITaskQueue::TThreadStats snap;
        taskQueue->CollectStats(i, snap);
        ready += snap.InternalTasksReady.load(std::memory_order_relaxed);
    }
    return ready;
}

} // anonymous

bool ObserveSchedulerInflightStuck(
    TInflightStuckState& state,
    size_t inflight,
    size_t ready,
    size_t threadCount,
    size_t maxInflight)
{
    const bool hasHeadroom = threadCount > 0
        && maxInflight > threadCount
        && (maxInflight >= threadCount * 2 || maxInflight >= threadCount + 8);
    const size_t readyThreshold = std::max(kInflightStuckMinReadyBacklog, threadCount * 4);
    const bool glued = hasHeadroom
        && inflight > 0
        && inflight <= threadCount
        && ready >= readyThreshold;
    if (!glued) {
        state.ConsecutiveGlued = 0;
        return false;
    }
    ++state.ConsecutiveGlued;
    if (state.Warned || state.ConsecutiveGlued < kInflightStuckMinConsecutiveSamples) {
        return false;
    }
    state.Warned = true;
    return true;
}

uint64_t PercentileToMilliseconds(uint64_t value, const char* unit) {
    if (unit != nullptr && std::strcmp(unit, "us") == 0) {
        return value / 1000;
    }
    return value;
}

void CollectLatencyConstraintViolations(
    const TTerminalStats& aggregated,
    const char* unit,
    std::vector<TLatencyConstraintViolation>& out)
{
    out.clear();
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        const auto completed = s.OK.load(std::memory_order_relaxed)
            + s.UserAborted.load(std::memory_order_relaxed);
        if (completed == 0) {
            continue;
        }
        const uint64_t p90Ms = PercentileToMilliseconds(
            s.LatencyHistogramFullMs.GetValueAtPercentile(90), unit);
        const uint64_t limitMs = TpccP90LimitMs(type);
        if (p90Ms >= limitMs) {
            out.push_back(TLatencyConstraintViolation{
                TransactionTypeName(type), p90Ms, limitMs});
        }
    }
}

std::string FormatInvalidRunLatencyBanner(
    const std::vector<TLatencyConstraintViolation>& violations)
{
    if (violations.empty()) {
        return {};
    }
    std::string out;
    out += "************************************************************************\n";
    out += "*** INVALID RUN: TPC-C 5.11 response-time constraints not met\n";
    for (const auto& v : violations) {
        out += fmt::format("***   {} p90={}ms exceeds {}ms (Clause 5.2.5.3)\n",
            v.TypeName, v.P90Ms, v.LimitMs);
    }
    out += "************************************************************************\n";
    return out;
}

static void LogInvalidRunLatencyBanner(const std::vector<TLatencyConstraintViolation>& violations) {
    const auto banner = FormatInvalidRunLatencyBanner(violations);
    if (banner.empty()) {
        return;
    }
    size_t begin = 0;
    while (begin < banner.size()) {
        const auto end = banner.find('\n', begin);
        const auto line = end == std::string::npos
            ? banner.substr(begin)
            : banner.substr(begin, end - begin);
        if (!line.empty()) {
            LOG_I(line);
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
}

TRunLayout ComputeRunLayout(const TRunSizingInput& input) {
    TRunLayout layout;
    layout.Ranges = input.WarehouseRanges;
    if (layout.Ranges.empty()) {
        layout.Ranges.push_back(
            TWarehouseRange{1, static_cast<int>(input.WarehouseCount) + 1});
    }
    layout.WarehouseCount = CountWarehouses(layout.Ranges);
    layout.ScaleWarehouses =
        input.ScaleWarehouses > 0 ? input.ScaleWarehouses : layout.WarehouseCount;
    layout.TerminalsPerWarehouse = input.Workload.TerminalsPerWarehouse > 0
        ? input.Workload.TerminalsPerWarehouse
        : TERMINALS_PER_WAREHOUSE;
    layout.TerminalCount = layout.WarehouseCount * layout.TerminalsPerWarehouse;

    layout.MaxInflight = input.MaxInflight;
    if (layout.MaxInflight == 0) {
        throw std::runtime_error("MaxInflight must be greater than zero");
    }
    layout.PoolSize = std::min(layout.TerminalCount, layout.MaxInflight);

    // Resolve ioThreads early so we can reserve CPU for them when sizing the
    // terminal thread pool below.
    layout.IoThreads = input.IOThreads;
    if (layout.IoThreads == 0) {
        layout.IoThreads = layout.MaxInflight;
    }
    layout.IoThreads = std::max(layout.IoThreads, layout.PoolSize);

    const size_t cpuCount = NumberOfMyCpus();
    const size_t reservedForIo =
        std::min(layout.IoThreads, std::max<size_t>(cpuCount / 4, 1));
    const size_t maxTerminalThreadCountAvailable =
        cpuCount > reservedForIo ? cpuCount - reservedForIo : 1;

    layout.RecommendedThreadCount =
        (layout.WarehouseCount + WAREHOUSES_PER_CPU_CORE - 1) / WAREHOUSES_PER_CPU_CORE;

    size_t threadCount;
    if (input.ThreadCount == 0) {
        threadCount = std::min(maxTerminalThreadCountAvailable, layout.TerminalCount);
        threadCount = std::min(threadCount, layout.RecommendedThreadCount);

        // Prefer an even thread count when there is still CPU headroom.
        if (threadCount % 2 != 0 && threadCount < maxTerminalThreadCountAvailable) {
            ++threadCount;
        }
    } else {
        threadCount = input.ThreadCount;
        if (threadCount > maxTerminalThreadCountAvailable) {
            LOG_I("User provided thread count " << threadCount
                  << " is above max available " << maxTerminalThreadCountAvailable << " "
                  << "(cpu count " << cpuCount << ", io threads " << layout.IoThreads
                  << "). Recommended for " << layout.WarehouseCount
                  << " warehouses is " << layout.RecommendedThreadCount << ". "
                  << "Setting thread count to " << maxTerminalThreadCountAvailable);
            threadCount = maxTerminalThreadCountAvailable;
        }
    }
    threadCount = std::max(threadCount, size_t(1));
    layout.ThreadCount = threadCount;

    if (layout.ThreadCount < layout.RecommendedThreadCount) {
        LOG_W("Thread count " << layout.ThreadCount
              << " is lower than recommended " << layout.RecommendedThreadCount << ". "
              << "It might affect benchmark results");
    }

    return layout;
}

TPhaseDurationResult ResolvePhaseDurations(const TPhaseDurationInput& input) {
    constexpr auto MinWarmupPerTerminalMs = std::chrono::milliseconds(1);
    const uint32_t minWarmupSeconds = static_cast<uint32_t>(
        input.TerminalCount * MinWarmupPerTerminalMs.count() / 1000 + 1);

    TPhaseDurationResult result;
    auto& durations = result.Durations;

    if (input.Orchestrated || input.HasStartAt) {
        durations.RampUpMs = input.PhasePolicy.RampUpMs;
        durations.MeasurementMs = input.PhasePolicy.MeasurementMs;
        durations.TransactionDrainMs = input.PhasePolicy.TransactionDrainMs;
        durations.StopGraceMs = input.PhasePolicy.StopGraceMs;
        if (durations.MeasurementMs <= 0 && !input.Orchestrated) {
            durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                input.RunDuration).count();
        }
    } else if (input.SkipWarmup) {
        durations.RampUpMs = 0;
        durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            input.RunDuration).count();
    } else {
        uint32_t warmupSeconds = 0;
        if (input.WarmupDuration.count() == 0) {
            if (input.WarehouseCount <= 10) {
                warmupSeconds = 30;
            } else if (input.WarehouseCount <= 100) {
                warmupSeconds = 5 * 60;
            } else if (input.WarehouseCount <= 1000) {
                warmupSeconds = 10 * 60;
            } else {
                warmupSeconds = 30 * 60;
            }
            warmupSeconds = std::max(warmupSeconds, minWarmupSeconds);
        } else {
            warmupSeconds = static_cast<uint32_t>(input.WarmupDuration.count());
            if (warmupSeconds < minWarmupSeconds) {
                result.ForcedWarmup = true;
                warmupSeconds = minWarmupSeconds;
            }
        }
        durations.RampUpMs = static_cast<int64_t>(warmupSeconds) * 1000;
        durations.MeasurementMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            input.RunDuration).count();
    }

    return result;
}

EStartAtWaitResult WaitUntilStartAt(
    std::chrono::system_clock::time_point rampStart,
    std::chrono::system_clock::time_point preparedAt,
    std::stop_token stopToken)
{
    using SysClock = std::chrono::system_clock;

    if (preparedAt >= rampStart) {
        LOG_E("Missed --start-at deadline " << FormatRfc3339Utc(rampStart)
              << ": prepare finished at " << FormatRfc3339Utc(preparedAt));
        GetGlobalErrorVariable().store(true);
        GetGlobalInterruptSource().request_stop();
        return EStartAtWaitResult::MissedDeadline;
    }

    LOG_I("Prepared; waiting until start-at " << FormatRfc3339Utc(rampStart) << " ("
          << std::chrono::duration_cast<std::chrono::milliseconds>(rampStart - preparedAt).count()
          << " ms)");
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
        return EStartAtWaitResult::Interrupted;
    }
    return EStartAtWaitResult::Ok;
}

std::string FormatProgressTransactionFields(
    const TTerminalStats& aggregated,
    const std::array<size_t, TRANSACTION_TYPE_COUNT>& cumulativeCompleted,
    std::array<size_t, TRANSACTION_TYPE_COUNT>& lastCompleted,
    const char* unit)
{
    std::string latencies;
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        const size_t cumulative = cumulativeCompleted[i];
        const size_t previous = lastCompleted[i];
        const size_t delta = cumulative >= previous ? cumulative - previous : cumulative;
        lastCompleted[i] = cumulative;
        if (cumulative == 0) {
            continue;
        }

        const auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        // Phase p90, not the last interval: TPC-C 5.2.5.3 is defined on the
        // measurement window. Warmup samples stay in ProgressLatencyFullMs.
        const uint64_t p90 = s.LatencyHistogramFullMs.TotalCount() > 0
            ? s.LatencyHistogramFullMs.GetValueAtPercentile(90)
            : s.ProgressLatencyFullMs.GetValueAtPercentile(90);
        latencies += fmt::format("  {}:{}(p90={})",
            TransactionTypeName(type), delta, FormatPercentile(p90, unit));
    }
    return latencies;
}

void MaybeUpdateConsoleStats(
    TProgressDisplayState& state,
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    ERunPhase phase,
    const TPhaseSchedule& schedule,
    Clock::time_point rampStartSteady,
    Clock::time_point measureStartSteady,
    Clock::time_point measureEndSteady,
    ITaskQueue* taskQueue)
{
    using SysClock = std::chrono::system_clock;

    if (phase == ERunPhase::Measure || phase == ERunPhase::Drain) {
        bool progressReset = false;
        for (auto& stats : perThreadStats) {
            // Call every thread: `||` must not skip the rest once one clears.
            progressReset = stats->ClearProgressOnce() || progressReset;
        }
        if (progressReset) {
            state.LastProgressCompleted.fill(0);
        }
    }

    auto now = Clock::now();
    if (!ShouldUpdateConsoleStats(state.LastUpdate, now, config.StatsInterval)) {
        return;
    }

    const auto wallNow = SysClock::now();
    double elapsed = 0.0;
    double phaseTotal = 0.0;
    double remaining = 0.0;

    switch (phase) {
        case ERunPhase::Ramp: {
            elapsed = std::chrono::duration<double>(now - rampStartSteady).count();
            phaseTotal = std::chrono::duration<double>(measureStartSteady - rampStartSteady).count();
            remaining = std::chrono::duration<double>(schedule.MeasurementStart - wallNow).count();
            break;
        }
        case ERunPhase::Measure: {
            elapsed = std::chrono::duration<double>(now - measureStartSteady).count();
            phaseTotal = std::chrono::duration<double>(measureEndSteady - measureStartSteady).count();
            remaining = std::chrono::duration<double>(schedule.MeasurementEnd - wallNow).count();
            break;
        }
        case ERunPhase::Drain: {
            elapsed = std::chrono::duration<double>(now - measureEndSteady).count();
            phaseTotal = std::chrono::duration<double>(
                schedule.DrainDeadline - schedule.MeasurementEnd).count();
            remaining = std::chrono::duration<double>(schedule.DrainDeadline - wallNow).count();
            break;
        }
        default: {
            elapsed = std::chrono::duration<double>(now - rampStartSteady).count();
            phaseTotal = elapsed;
            remaining = 0.0;
            break;
        }
    }
    if (remaining < 0.0) {
        remaining = 0.0;
    }
    if (phaseTotal < 0.0) {
        phaseTotal = 0.0;
    }

    size_t totalFailed = 0;
    size_t totalNewOrderCompleted = 0;
    std::array<size_t, TRANSACTION_TYPE_COUNT> cumulativeCompleted{};
    uint64_t aggHdr = 0;
    uint64_t aggMax = 0;
    bool aggUs = false;
    ResolveHistogramParams(config.Histogram, config.HighResHistogram, aggHdr, aggMax, aggUs);
    TTerminalStats aggregated(aggHdr, aggMax, aggUs);

    for (auto& stats : perThreadStats) {
        stats->Collect(aggregated);
        stats->CollectProgressLatency(aggregated);
        for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
            const auto& s = stats->GetStats(static_cast<ETransactionType>(i));
            cumulativeCompleted[i] += s.ProgressOK.load(std::memory_order_relaxed)
                + s.ProgressUserAborted.load(std::memory_order_relaxed);
            totalFailed += s.ProgressFailed.load(std::memory_order_relaxed);
        }
        const auto& no = stats->GetStats(ETransactionType::NewOrder);
        totalNewOrderCompleted += no.ProgressOK.load(std::memory_order_relaxed)
            + no.ProgressUserAborted.load(std::memory_order_relaxed);
    }

    // tpmC uses live Progress* over time spent in the current phase.
    double tpmc = elapsed > 0 ? (totalNewOrderCompleted / elapsed * 60.0) : 0.0;
    double efficiency = config.WarehouseCount > 0
        ? (tpmc / (MAX_TPMC_PER_WAREHOUSE * config.WarehouseCount) * 100.0) : 0.0;

    const char* unit = HistogramUnitLabel(config);
    const std::string latencies = FormatProgressTransactionFields(
        aggregated, cumulativeCompleted, state.LastProgressCompleted, unit);

    const size_t inflight = TransactionsInflight.load(std::memory_order_relaxed);
    if (config.NoDelays) {
        LOG_I(fmt::format("{} {:.0f}s/{:.0f}s left | tpmC:{:.0f} | Fail:{} Inflight:{} |{}{}",
              RunPhaseName(phase), remaining, phaseTotal, tpmc,
              totalFailed,
              inflight,
              latencies, FormatSchedulerStats(taskQueue)));
    } else {
        LOG_I(fmt::format("{} {:.0f}s/{:.0f}s left | tpmC:{:.0f} eff:{:.1f}% | Fail:{} Inflight:{} |{}{}",
              RunPhaseName(phase), remaining, phaseTotal, tpmc, efficiency,
              totalFailed,
              inflight,
              latencies, FormatSchedulerStats(taskQueue)));
    }

    if (phase == ERunPhase::Ramp || phase == ERunPhase::Measure) {
        const size_t ready = SchedulerReadyCount(taskQueue);
        if (ObserveSchedulerInflightStuck(
                state.InflightStuck, inflight, ready, config.ThreadCount, config.MaxInflight))
        {
            const auto stuckWindow = config.StatsInterval * kInflightStuckMinConsecutiveSamples;
            const auto stuckSeconds =
                std::chrono::duration_cast<std::chrono::seconds>(stuckWindow).count();
            LOG_W("Inflight=" << inflight
                  << " stayed near ThreadCount=" << config.ThreadCount
                  << " for ~" << stuckSeconds << "s while max_inflight="
                  << config.MaxInflight << " (ready=" << ready
                  << "). ITpccTransaction may be blocking the scheduler; see "
                  << "docs/async-adapter-transactions.md");
        }
    }

    state.LastUpdate = now;
}

void PrintFinalResults(
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    std::chrono::duration<double> measureElapsed,
    ITaskQueue* taskQueue)
{
    uint64_t aggHdr = 0;
    uint64_t aggMax = 0;
    bool aggUs = false;
    ResolveHistogramParams(config.Histogram, config.HighResHistogram, aggHdr, aggMax, aggUs);
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
    const char* unit = HistogramUnitLabel(config);
    std::vector<TLatencyConstraintViolation> latencyViolations;
    CollectLatencyConstraintViolations(aggregated, unit, latencyViolations);
    LogInvalidRunLatencyBanner(latencyViolations);
    LOG_I(fmt::format("  Measured Duration: {:.1f}s (configured: {}s)",
          measureDuration, config.RunDuration.count()));
    LOG_I(fmt::format("  New-Order Throughput: {:.2f} tpmC", tpmc));
    if (!config.NoDelays) {
        LOG_I(fmt::format("  Efficiency: {:.1f}%", efficiency));
    }
    if (!latencyViolations.empty()) {
        LOG_I("  *** INVALID RUN (latency constraints not met)");
    }
    LOG_I("  Total Failed: " << totalFailed);
    if (taskQueue) {
        const auto sched = FormatSchedulerStats(taskQueue);
        if (!sched.empty()) {
            LOG_I(fmt::format("  Scheduler:{}", sched));
        }
    }

    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        auto ok = s.OK.load(std::memory_order_relaxed);
        auto failed = s.Failed.load(std::memory_order_relaxed);
        auto userAborted = s.UserAborted.load(std::memory_order_relaxed);
        if (ok == 0 && failed == 0 && userAborted == 0) continue;

        LOG_I("  " << TransactionTypeName(type) << ": OK=" << ok
              << " UserAborted=" << userAborted << " Failed=" << failed
              << " p50=" << FormatPercentile(s.LatencyHistogramFullMs.GetValueAtPercentile(50), unit)
              << " p90=" << FormatPercentile(s.LatencyHistogramFullMs.GetValueAtPercentile(90), unit)
              << " p99=" << FormatPercentile(s.LatencyHistogramFullMs.GetValueAtPercentile(99), unit));
    }
}

void RunMeasurementDrainLoop(
    TPhaseController& phaseController,
    const TPhaseSchedule& schedule,
    bool asyncDelivery,
    std::stop_token stopToken,
    std::chrono::milliseconds sleepEvery,
    std::function<void(ERunPhase phase)> maybeUpdateDisplay)
{
    using SysClock = std::chrono::system_clock;

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

        if (maybeUpdateDisplay) {
            maybeUpdateDisplay(phase);
        }
        std::this_thread::sleep_for(sleepEvery);
    }

    phaseController.SetPhase(ERunPhase::Stop);
}

} // namespace NTpcc
