#pragma once

#include "run_config_document.h"

#include <constants.h>
#include <error_classifier.h>
#include <session.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace NTpcc {

inline constexpr int kDefaultDebugRepeats = 10;

const char* TransactionTypeJsonName(ETransactionType type);

struct TDurationStats {
    int64_t First = 0;
    int64_t Min = 0;
    int64_t Max = 0;
    int64_t Avg = 0;
    std::optional<int64_t> RestAvg;
};

TDurationStats ComputeDurationStats(const std::vector<int64_t>& samples);

struct TDebugAttempt {
    int N = 0;
    std::string Status; // ok | user_aborted | failed
    int64_t DurationUs = 0;
    int64_t LatencyPureUs = 0;
    std::string Error;
};

struct TDebugTxReport {
    std::string Type;
    int Ok = 0;
    int Failed = 0;
    int UserAborted = 0;
    std::vector<TDebugAttempt> Attempts;
    TDurationStats DurationUs;
    TDurationStats LatencyPureUs;
};

struct TDebugReport {
    int SchemaVersion = 1;
    std::string Role = "debug";
    std::string RunId;
    std::string Instance;
    int Repeats = kDefaultDebugRepeats;
    size_t WarehouseID = 1;
    size_t WarehouseCount = 1;
    size_t DistrictID = 1;
    bool Ok = true;
    std::vector<TDebugTxReport> Transactions;

    int FailedCount() const;
};

struct TDebugProbeRequest {
    ISessionFactory* SessionFactory = nullptr;
    IErrorClassifier* ErrorClassifier = nullptr;
    EIsolationLevel Isolation = EIsolationLevel::RepeatableRead;
    size_t WarehouseID = 1;
    size_t WarehouseCount = 1;
    size_t DistrictID = 1;
    int NewOrderRemoteWarehousePercent = NEW_ORDER_REMOTE_WAREHOUSE_PERCENT;
    int PaymentRemoteWarehousePercent = PAYMENT_REMOTE_WAREHOUSE_PERCENT;
    int NewOrderMaxRemoteWarehouses = NEW_ORDER_MAX_REMOTE_WAREHOUSES;
    int Repeats = kDefaultDebugRepeats;
    std::string RunId;
    std::string Instance;
};

// Sequential one-at-a-time probe: each TPC-C transaction type, Repeats times.
// Prints duration lines to stdout and returns a structured report.
TDebugReport RunDebugProbe(const TDebugProbeRequest& request);

void WriteDebugReportJson(const std::string& path, const TDebugReport& report);
std::string DebugReportToJson(const TDebugReport& report);

// Orchestrated debug role (process.json / probe.json / artifact-manifest.json).
int RunOrchestratedDebug(
    const TRunConfigDocument& doc,
    const std::string& instance,
    int repeats,
    std::function<TDebugReport(const TRunConfigDocument&, TDebugProbeRequest)> runProbe);

} // namespace NTpcc
