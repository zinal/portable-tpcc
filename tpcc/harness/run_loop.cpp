#include "run_loop.h"

#include <constants.h>
#include <domain_util.h>
#include <log.h>
#include <time_util.h>
#include <context.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace NTpcc {

namespace {

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

// Console-friendly percentile: keep histogram native units, but when unit is us
// also show approximate milliseconds so bucket bounds like 65536 are readable.
std::string FormatPercentile(uint64_t value, const char* unit) {
    if (std::strcmp(unit, "us") == 0) {
        const double ms = static_cast<double>(value) / 1000.0;
        return fmt::format("{}us ({:.1f}ms)", value, ms);
    }
    return fmt::format("{}ms", value);
}

} // anonymous

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

void MaybeUpdateConsoleStats(
    TProgressDisplayState& state,
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    ERunPhase phase,
    const TPhaseSchedule& schedule,
    Clock::time_point rampStartSteady,
    Clock::time_point measureStartSteady,
    Clock::time_point measureEndSteady)
{
    using SysClock = std::chrono::system_clock;

    if (phase == ERunPhase::Measure || phase == ERunPhase::Drain) {
        for (auto& stats : perThreadStats) {
            stats->ClearProgressOnce();
        }
    }

    auto now = Clock::now();
    auto sinceLast = std::chrono::duration_cast<std::chrono::seconds>(now - state.LastUpdate);
    if (state.LastUpdate.time_since_epoch().count() != 0 && sinceLast < std::chrono::seconds(5)) {
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

    size_t totalOK = 0;
    size_t totalFailed = 0;
    size_t totalNewOrderCompleted = 0;
    uint64_t aggHdr = 0;
    uint64_t aggMax = 0;
    bool aggUs = false;
    ResolveHistogramParams(config.Histogram, config.HighResHistogram, aggHdr, aggMax, aggUs);
    TTerminalStats aggregated(aggHdr, aggMax, aggUs);

    for (auto& stats : perThreadStats) {
        stats->Collect(aggregated);
        for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
            const auto& s = stats->GetStats(static_cast<ETransactionType>(i));
            totalOK += s.ProgressOK.load(std::memory_order_relaxed);
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
    std::string latencies;
    for (size_t i = 0; i < TRANSACTION_TYPE_COUNT; ++i) {
        auto type = static_cast<ETransactionType>(i);
        const auto& s = aggregated.GetStats(type);
        auto p50 = s.LatencyHistogramFullMs.GetValueAtPercentile(50);
        auto p99 = s.LatencyHistogramFullMs.GetValueAtPercentile(99);
        // Latencies are measurement-window only; during ramp show progress counts.
        auto completed = s.OK.load(std::memory_order_relaxed)
            + s.UserAborted.load(std::memory_order_relaxed);
        size_t progressCompleted = 0;
        for (auto& stats : perThreadStats) {
            const auto& ps = stats->GetStats(type);
            progressCompleted += ps.ProgressOK.load(std::memory_order_relaxed)
                + ps.ProgressUserAborted.load(std::memory_order_relaxed);
        }
        if (progressCompleted > 0) {
            if (completed > 0) {
                latencies += fmt::format("  {}:{}(p50={} p99={})",
                    TransactionTypeName(type), progressCompleted,
                    FormatPercentile(p50, unit), FormatPercentile(p99, unit));
            } else {
                latencies += fmt::format("  {}:{}", TransactionTypeName(type), progressCompleted);
            }
        }
    }

    if (config.NoDelays) {
        LOG_I(fmt::format("{} {:.0f}s/{:.0f}s ({:.0f}s left) | tpmC:{:.0f} | OK:{} Fail:{} Inflight:{} |{}",
              RunPhaseName(phase), elapsed, phaseTotal, remaining, tpmc,
              totalOK, totalFailed,
              TransactionsInflight.load(std::memory_order_relaxed),
              latencies));
    } else {
        LOG_I(fmt::format("{} {:.0f}s/{:.0f}s ({:.0f}s left) | tpmC:{:.0f} eff:{:.1f}% | OK:{} Fail:{} Inflight:{} |{}",
              RunPhaseName(phase), elapsed, phaseTotal, remaining, tpmc, efficiency,
              totalOK, totalFailed,
              TransactionsInflight.load(std::memory_order_relaxed),
              latencies));
    }

    state.LastUpdate = now;
}

void PrintFinalResults(
    const TRunStatsConfig& config,
    const std::vector<std::shared_ptr<TTerminalStats>>& perThreadStats,
    std::chrono::duration<double> measureElapsed)
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
    LOG_I(fmt::format("  Measured Duration: {:.1f}s (configured: {}s)",
          measureDuration, config.RunDuration.count()));
    LOG_I(fmt::format("  New-Order Throughput: {:.2f} tpmC", tpmc));
    if (!config.NoDelays) {
        LOG_I(fmt::format("  Efficiency: {:.1f}%", efficiency));
    }
    LOG_I("  Total Failed: " << totalFailed);

    const char* unit = HistogramUnitLabel(config);
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
