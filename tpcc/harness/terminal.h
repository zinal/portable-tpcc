#pragma once

#include <task_queue.h>
#include <constants.h>
#include <histogram.h>
#include <phase_controller.h>
#include <workload_config.h>
#include <error_classifier.h>
#include <session.h>
#include <workflows.h>

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
    struct TLatencySample {
        std::chrono::microseconds Transaction{0};
        std::chrono::microseconds Full{0};
        std::chrono::microseconds Pure{0};
        std::chrono::microseconds AdmissionWait{0};
        std::chrono::microseconds SessionPoolWait{0};
        std::chrono::microseconds RetryBackoff{0};
    };

    struct TTransactionStats {
        explicit TTransactionStats(uint64_t hdrTill = 4096, uint64_t maxValue = 32768)
            : LatencyHistogramMs(hdrTill, maxValue)
            , LatencyHistogramFullMs(hdrTill, maxValue)
            , LatencyHistogramPure(hdrTill, maxValue)
            , LatencyHistogramAdmission(hdrTill, maxValue)
            , LatencyHistogramSessionPool(hdrTill, maxValue)
            , LatencyHistogramRetryBackoff(hdrTill, maxValue)
            , ProgressLatencyFullMs(hdrTill, maxValue)
        {}

        void ResetHistograms(uint64_t hdrTill, uint64_t maxValue) {
            std::lock_guard guard(HistLock);
            LatencyHistogramMs = THistogram(hdrTill, maxValue);
            LatencyHistogramFullMs = THistogram(hdrTill, maxValue);
            LatencyHistogramPure = THistogram(hdrTill, maxValue);
            LatencyHistogramAdmission = THistogram(hdrTill, maxValue);
            LatencyHistogramSessionPool = THistogram(hdrTill, maxValue);
            LatencyHistogramRetryBackoff = THistogram(hdrTill, maxValue);
            ProgressLatencyFullMs = THistogram(hdrTill, maxValue);
        }

        void Collect(TTransactionStats& dst) const {
            dst.OK.fetch_add(OK.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.Failed.fetch_add(Failed.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.UserAborted.fetch_add(UserAborted.load(std::memory_order_relaxed), std::memory_order_relaxed);
            dst.Retried.fetch_add(Retried.load(std::memory_order_relaxed), std::memory_order_relaxed);
            // Progress* and ProgressLatencyFullMs are console-only and must not
            // flow into measurement aggregates.

            std::lock_guard guard(HistLock);
            dst.LatencyHistogramMs.Add(LatencyHistogramMs);
            dst.LatencyHistogramFullMs.Add(LatencyHistogramFullMs);
            dst.LatencyHistogramPure.Add(LatencyHistogramPure);
            dst.LatencyHistogramAdmission.Add(LatencyHistogramAdmission);
            dst.LatencyHistogramSessionPool.Add(LatencyHistogramSessionPool);
            dst.LatencyHistogramRetryBackoff.Add(LatencyHistogramRetryBackoff);
        }

        void Clear() {
            OK.store(0, std::memory_order_relaxed);
            Failed.store(0, std::memory_order_relaxed);
            UserAborted.store(0, std::memory_order_relaxed);
            Retried.store(0, std::memory_order_relaxed);
            ClearProgress();

            std::lock_guard guard(HistLock);
            LatencyHistogramMs.Reset();
            LatencyHistogramFullMs.Reset();
            LatencyHistogramPure.Reset();
            LatencyHistogramAdmission.Reset();
            LatencyHistogramSessionPool.Reset();
            LatencyHistogramRetryBackoff.Reset();
        }

        void ClearProgress() {
            ProgressOK.store(0, std::memory_order_relaxed);
            ProgressFailed.store(0, std::memory_order_relaxed);
            ProgressUserAborted.store(0, std::memory_order_relaxed);
            std::lock_guard guard(HistLock);
            ProgressLatencyFullMs.Reset();
        }

        // Measurement-window counters (TPC-C §5.4.2); used for final results.
        std::atomic<size_t> OK = 0;
        std::atomic<size_t> Failed = 0;
        std::atomic<size_t> UserAborted = 0;
        std::atomic<size_t> Retried = 0;
        // Live console counters (ramp + measure). Reset when measure starts.
        // The progress line prints per-type increments of OK+UserAborted, not these sums.
        std::atomic<size_t> ProgressOK = 0;
        std::atomic<size_t> ProgressFailed = 0;
        std::atomic<size_t> ProgressUserAborted = 0;

        mutable TSpinLock HistLock;
        THistogram LatencyHistogramMs;
        THistogram LatencyHistogramFullMs;
        THistogram LatencyHistogramPure;
        THistogram LatencyHistogramAdmission;
        THistogram LatencyHistogramSessionPool;
        THistogram LatencyHistogramRetryBackoff;
        // Full response time for the console p90. Includes warmup, which must
        // not enter LatencyHistogramFullMs. Cleared with the progress counters.
        THistogram ProgressLatencyFullMs;
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

    void AddOK(ETransactionType type, const TLatencySample& sample)
    {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.OK.fetch_add(1, std::memory_order_relaxed);
        RecordLatency(stats, sample);
    }

    void IncFailed(ETransactionType type) {
        PerTransactionTypeStats[static_cast<size_t>(type)].Failed.fetch_add(1, std::memory_order_relaxed);
    }

    // Intentional profile rollback (unused New-Order item). Counts toward MQTh/tpmC
    // and New-Order response-time statistics (TPC-C §5.1.2, §5.4.2).
    void AddUserAborted(ETransactionType type, const TLatencySample& sample)
    {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.UserAborted.fetch_add(1, std::memory_order_relaxed);
        RecordLatency(stats, sample);
    }

    void IncRetried(ETransactionType type) {
        PerTransactionTypeStats[static_cast<size_t>(type)].Retried.fetch_add(1, std::memory_order_relaxed);
    }

    void AddProgressOK(ETransactionType type, std::chrono::microseconds fullLatency) {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.ProgressOK.fetch_add(1, std::memory_order_relaxed);
        RecordProgressLatency(stats, fullLatency);
    }

    void IncProgressFailed(ETransactionType type) {
        PerTransactionTypeStats[static_cast<size_t>(type)].ProgressFailed.fetch_add(
            1, std::memory_order_relaxed);
    }

    void AddProgressUserAborted(ETransactionType type, std::chrono::microseconds fullLatency) {
        auto& stats = PerTransactionTypeStats[static_cast<size_t>(type)];
        stats.ProgressUserAborted.fetch_add(1, std::memory_order_relaxed);
        RecordProgressLatency(stats, fullLatency);
    }

    void Collect(TTerminalStats& dst) const {
        for (size_t i = 0; i < PerTransactionTypeStats.size(); ++i) {
            PerTransactionTypeStats[i].Collect(dst.PerTransactionTypeStats[i]);
        }
    }

    // Console-only full-latency histogram. Separate from Collect() so warmup
    // samples cannot enter result.json / measurement percentiles.
    void CollectProgressLatency(TTerminalStats& dst) const {
        for (size_t i = 0; i < PerTransactionTypeStats.size(); ++i) {
            const auto& src = PerTransactionTypeStats[i];
            auto& out = dst.PerTransactionTypeStats[i];
            std::lock_guard guard(src.HistLock);
            out.ProgressLatencyFullMs.Add(src.ProgressLatencyFullMs);
        }
    }

    void Clear() {
        for (auto& stats: PerTransactionTypeStats) {
            stats.Clear();
        }
        ProgressClearedForMeasure.store(false, std::memory_order_relaxed);
    }

    // Drop ramp live counters once when entering measurement so console tpmC
    // reflects the current phase only. Measurement OK/Fail are untouched.
    // Returns true on the call that actually cleared.
    bool ClearProgressOnce() {
        bool expected = false;
        if (ProgressClearedForMeasure.compare_exchange_strong(
                expected, true, std::memory_order_relaxed))
        {
            for (auto& stats : PerTransactionTypeStats) {
                stats.ClearProgress();
            }
            return true;
        }
        return false;
    }

private:
    uint64_t ToRecorded(std::chrono::microseconds value) const {
        int64_t n = value.count();
        if (n < 0) {
            n = 0;
        }
        if (RecordMicroseconds) {
            return static_cast<uint64_t>(n);
        }
        return static_cast<uint64_t>(n / 1000);
    }

    void RecordProgressLatency(TTransactionStats& stats, std::chrono::microseconds full) {
        const uint64_t vFull = ToRecorded(full);
        std::lock_guard guard(stats.HistLock);
        stats.ProgressLatencyFullMs.RecordValue(vFull);
    }

    void RecordLatency(TTransactionStats& stats, const TLatencySample& sample)
    {
        const uint64_t vTxn = ToRecorded(sample.Transaction);
        const uint64_t vFull = ToRecorded(sample.Full);
        const uint64_t vPure = ToRecorded(sample.Pure);
        const uint64_t vAdmission = ToRecorded(sample.AdmissionWait);
        const uint64_t vPool = ToRecorded(sample.SessionPoolWait);
        const uint64_t vBackoff = ToRecorded(sample.RetryBackoff);
        {
            std::lock_guard guard(stats.HistLock);
            stats.LatencyHistogramMs.RecordValue(vTxn);
            stats.LatencyHistogramFullMs.RecordValue(vFull);
            stats.LatencyHistogramPure.RecordValue(vPure);
            stats.LatencyHistogramAdmission.RecordValue(vAdmission);
            stats.LatencyHistogramSessionPool.RecordValue(vPool);
            stats.LatencyHistogramRetryBackoff.RecordValue(vBackoff);
        }
    }

    std::array<TTransactionStats, TRANSACTION_TYPE_COUNT> PerTransactionTypeStats;
    std::atomic<bool> ProgressClearedForMeasure{false};
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
        IErrorClassifier* errorClassifier,
        EIsolationLevel isolation,
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
    IErrorClassifier* ErrorClassifier;
    EIsolationLevel Isolation;
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
