#pragma once

#include <constants.h>

#include <array>
#include <cstdint>
#include <string>

namespace NTpcc {

// How think time is drawn from the configured per-tx mean (ThinkTimeMs).
// Exponential matches TPC-C §5.2.5.4. Benchbase keeps a constant mean wait
// (previous portable-tpcc behavior; optional engineering compatibility mode).
enum class EThinkTimeDistribution {
    Exponential,
    Benchbase,
};

// Per-transaction mix weight and pacing (from run-config workload.*).
struct TTxWorkload {
    double Weight = 0.0;
    int64_t KeyingTimeMs = 0;
    int64_t ThinkTimeMs = 0;
};

struct TWorkloadConfig {
    size_t TerminalsPerWarehouse = 0; // 0 → TERMINALS_PER_WAREHOUSE
    bool HasCustomMix = false;
    bool HasCustomKeying = false;
    bool HasCustomThink = false;
    std::array<TTxWorkload, TRANSACTION_TYPE_COUNT> PerTx{};
};

// Histogram settings from runtime.histogram. Mapped onto THistogram linear_exp layout.
struct THistogramConfig {
    bool Configured = false;
    std::string Unit = "ms"; // "ms" or "us"
    uint64_t Lowest = 1;
    uint64_t Highest = 32768;
    int SignificantFigures = 3;

    // Parameters for THistogram (values recorded in Unit).
    uint64_t HdrTill() const {
        if (!Configured) {
            return 4096;
        }
        // Linear region: prefer a power-of-two-ish window, capped by Highest.
        uint64_t till = 4096;
        if (Highest < till) {
            till = Highest > 0 ? Highest : 1;
        }
        return till;
    }

    uint64_t MaxValue() const {
        if (!Configured) {
            return 32768;
        }
        return Highest > 0 ? Highest : 32768;
    }
};

inline TWorkloadConfig MakeDefaultWorkloadConfig() {
    TWorkloadConfig w;
    w.TerminalsPerWarehouse = TERMINALS_PER_WAREHOUSE;
    w.PerTx[static_cast<size_t>(ETransactionType::NewOrder)] =
        {NEW_ORDER_WEIGHT, NEW_ORDER_KEYING_TIME.count() * 1000, NEW_ORDER_THINK_TIME.count() * 1000};
    w.PerTx[static_cast<size_t>(ETransactionType::Delivery)] =
        {DELIVERY_WEIGHT, DELIVERY_KEYING_TIME.count() * 1000, DELIVERY_THINK_TIME.count() * 1000};
    w.PerTx[static_cast<size_t>(ETransactionType::OrderStatus)] =
        {ORDER_STATUS_WEIGHT, ORDER_STATUS_KEYING_TIME.count() * 1000, ORDER_STATUS_THINK_TIME.count() * 1000};
    w.PerTx[static_cast<size_t>(ETransactionType::Payment)] =
        {PAYMENT_WEIGHT, PAYMENT_KEYING_TIME.count() * 1000, PAYMENT_THINK_TIME.count() * 1000};
    w.PerTx[static_cast<size_t>(ETransactionType::StockLevel)] =
        {STOCK_LEVEL_WEIGHT, STOCK_LEVEL_KEYING_TIME.count() * 1000, STOCK_LEVEL_THINK_TIME.count() * 1000};
    return w;
}

} // namespace NTpcc
