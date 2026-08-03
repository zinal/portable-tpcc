#pragma once

#include <task_queue.h>
#include <constants.h>
#include <histogram.h>
#include <phase_controller.h>
#include <workload_config.h>
#include <session.h>
#include "transactions.h"

#include <future.h>
#include <spinlock.h>

#include <atomic>
#include <chrono>
#include <stop_token>
#include <memory>
#include <array>
#include <string>

namespace NTpcc {

//-----------------------------------------------------------------------------

class TTerminalStats {
public:
    struct TTransactionStats {
        explicit TTransactionStats(uint64_t hdrTill = 4096, uint64_t maxValue = 32768)
            : LatencyHistogramMs(hdrTill, maxValue)
            , LatencyHistogramFullMs(hdrTill, maxValue)
            , LatencyHistogramPure(hdrTill, maxValue)
        {}

        void ResetHistograms(uint64_t hdrTill, uint64_t maxValue) {
            std::lock_guard guard(HistLock);
            LatencyHistogramMs = THistogram(hdrTill, maxValue);
            LatencyHistogramFullMs = THistogram(hdrTill, maxValue);
            LatencyHistogramPure = THistogram(hdrTill, maxValue);
        }

        void Collect(TTransactionStats& dst) const {
            dst.OK.fetch_add(OK.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.Failed.fetch_add(Failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.UserAborted.fetch_add(UserAborted.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.Retried.fetch_add(Retried.load(std::memory_order_relaxed), std::memory_order_relaxed);

            std::lock_guard guard(HistLock);
            dst.LatencyHistogramMs.Add(LatencyHistogramMs);
            dst.LatencyHistogramFullMs.Add(LatencyHistogramFullMs);
            dst.LatencyHistogramPure.Add(LatencyHistogramPure);
        }

        void Clear() {
            OK.store(0, std::memory_order_relaxed);
            Failed.store(0, std::memory_order_relaxed);
            UserAborted.store(0, std::memory_order_relaxed);
            Retried.store(0, std::memory_order_relaxed);

            std::lock_guard guard(HistLock);
            LatencyHistogramMs.Reset();
            LatencyHistogramFullMs.Reset();
            LatencyHistogramPure.Reset();
        }

        std::atomic<size_t> OK = 0;
        std::atomic<size_t> Failed = 0;
        std::atomic<size_t> UserAborted = 0;
        std::atomic<size_t> Retried = 0;

        mutable TSpinLock HistLock;
        THistogram LatencyHistogramMs;
        THistogram LatencyHistogramFullMs;
        THistogram LatencyHistogramPure;
    };

public:
    explicit TTerminalStats(
        uint64_t hdrTill = 4096,
        uint64_t maxValue = 32768,
        bool recordMicroseconds = false)
        : RecordMicroseconds(recordMicroseconds)
        , HdrTill_(hdrTill)
        , MaxValue_(maxValue)
    {
        for (auto& stats : PerTransactionTypeStats) {
            stats.ResetHistograms(hdrTill, maxValue);
        }
    }

    bool RecordsMicroseconds() const { return RecordMicroseconds; }
    uint64_t HdrTill() const { return HdrTill_; }
    uint64_t MaxValue() const { return MaxValue_; }

    const TTransactionStats& GetStats(ETransactionType type) const {
        return PerTransactionTypeStats[static_cast<size_t>(type)];
    }

    void AddOK(
        ETransactionType type,
        std::chrono::milliseconds latency,
        std::chrono::milliseconds latencyFull,
        std::chrono::microseconds latencyPure)
    {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.OK.fetch_add(1, std::memory_order_relaxed);
        RecordLatency(stats, latency, latencyFull, latencyPure);
    }

    void IncFailed(ETransactionType type) {
        PerTransactionTypeStats[static_cast<size_t>(type)].Failed.fetch_add(1, std::memory_order_relaxed);
    }

    // Intentional profile rollback (unused New-Order item). Counts toward MQTh/tpmC
    // and New-Order response-time statistics (TPC-C §5.1.2, §5.4.2).
    void AddUserAborted(
        ETransactionType type,
        std::chrono::milliseconds latency,
        std::chrono::milliseconds latencyFull,
        std::chrono::microseconds latencyPure)
    {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.UserAborted.fetch_add(1, std::memory_order_relaxed);
        RecordLatency(stats, latency, latencyFull, latencyPure);
    }

    void IncRetried(ETransactionType type) {
        PerTransactionTypeStats[static_cast<size_t>(type)].Retried.fetch_add(1, std::memory_order_relaxed);
    }

    void Collect(TTerminalStats& dst) const {
        for (size_t i = 0; i < PerTransactionTypeStats.size(); ++i) {
            PerTransactionTypeStats[i].Collect(dst.PerTransactionTypeStats[i]);
        }
    }

    void Clear() {
        for (auto& stats: PerTransactionTypeStats) {
            stats.Clear();
        }
    }

    void ClearOnce() {
        bool expected = false;
        if (WasCleared.compare_exchange_strong(expected, true, std::memory_order_relaxed)) {
            Clear();
        }
    }

private:
    void RecordLatency(
        TTransactionStats& stats,
        std::chrono::milliseconds latency,
        std::chrono::milliseconds latencyFull,
        std::chrono::microseconds latencyPure)
    {
        uint64_t vTxn = 0;
        uint64_t vFull = 0;
        uint64_t vPure = 0;
        if (RecordMicroseconds) {
            vTxn = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(latency).count());
            vFull = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(latencyFull).count());
            vPure = static_cast<uint64_t>(latencyPure.count());
        } else {
            vTxn = static_cast<uint64_t>(latency.count());
            vFull = static_cast<uint64_t>(latencyFull.count());
            vPure = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(latencyPure).count());
        }
        {
            std::lock_guard guard(stats.HistLock);
            stats.LatencyHistogramMs.RecordValue(vTxn);
            stats.LatencyHistogramFullMs.RecordValue(vFull);
            stats.LatencyHistogramPure.RecordValue(vPure);
        }
    }

    std::array<TTransactionStats, TRANSACTION_TYPE_COUNT> PerTransactionTypeStats;
    std::atomic<bool> WasCleared{false};
    bool RecordMicroseconds = false;
    uint64_t HdrTill_ = 4096;
    uint64_t MaxValue_ = 32768;
};

//-----------------------------------------------------------------------------

class alignas(64) TTerminal {
public:
    TTerminal(
        size_t terminalID,
        size_t warehouseID,
        size_t districtID,
        size_t warehouseCount,
        ITaskQueue& taskQueue,
        ISessionFactory* sessionFactory,
        bool noDelays,
        std::stop_token stopToken,
        TPhaseController& phaseController,
        std::shared_ptr<TTerminalStats>& stats,
        const TWorkloadConfig& workload,
        int simulateTransactionSelect1 = 0,
        size_t retryMaxAttempts = 4,
        int64_t retryInitialBackoffMs = 10,
        int64_t retryMaxBackoffMs = 500,
        std::string retryJitter = "full",
        bool retryAmbiguousCommit = false,
        EThinkTimeDistribution thinkTimeDistribution = EThinkTimeDistribution::Exponential);

    TTerminal(const TTerminal&) = delete;
    TTerminal& operator=(TTerminal&) = delete;
    TTerminal(TTerminal&&) = delete;
    TTerminal& operator=(TTerminal&&) = delete;

    size_t GetID() const { return Context.TerminalID; }

    void Start();
    bool IsDone() const { return Done.load(std::memory_order_relaxed); }

private:
    TFuture<void> Run();

private:
    ITaskQueue& TaskQueue;
    ISessionFactory* SessionFactory;
    TTransactionContext Context;
    bool NoDelays;
    std::stop_token StopToken;
    TPhaseController& PhaseController;
    std::shared_ptr<TTerminalStats> Stats;
    TWorkloadConfig Workload;
    size_t RetryMaxAttempts = 4;
    int64_t RetryInitialBackoffMs = 10;
    int64_t RetryMaxBackoffMs = 500;
    std::string RetryJitter = "full";
    bool RetryAmbiguousCommit = false;
    EThinkTimeDistribution ThinkTimeDistribution = EThinkTimeDistribution::Exponential;

    std::atomic<bool> Done{false};
    bool Started = false;
};

} // namespace NTpcc
