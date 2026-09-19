#include "debug_probe.h"

#include "artifacts.h"

#include <constants.h>
#include <coro_traits.h>
#include <domain_util.h>
#include <future.h>
#include <log.h>
#include <task_queue.h>
#include <workflows.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <unistd.h>
#include <utility>

namespace fs = std::filesystem;

namespace NTpcc {

namespace {

using Json = nlohmann::json;

struct TDebugTxDesc {
    ETransactionType Type;
    const char* JsonName;
    TFuture<bool> (*TaskFunc)(TTransactionContext&, std::chrono::microseconds&, ITpccTransaction&);
};

constexpr std::array<TDebugTxDesc, TRANSACTION_TYPE_COUNT> kDebugTransactions{{
    {ETransactionType::NewOrder, "new_order", &GetNewOrderTask},
    {ETransactionType::Payment, "payment", &GetPaymentTask},
    {ETransactionType::OrderStatus, "order_status", &GetOrderStatusTask},
    {ETransactionType::Delivery, "delivery", &GetDeliveryTask},
    {ETransactionType::StockLevel, "stock_level", &GetStockLevelTask},
}};

void InterruptHandler(int) {
    GetGlobalInterruptSource().request_stop();
}

Json DurationStatsJson(const TDurationStats& stats) {
    Json j = {
        {"first", stats.First},
        {"min", stats.Min},
        {"max", stats.Max},
        {"avg", stats.Avg},
    };
    if (stats.RestAvg.has_value()) {
        j["rest_avg"] = *stats.RestAvg;
    }
    return j;
}

void FinalizeTxReport(TDebugTxReport& tx) {
    std::vector<int64_t> durations;
    std::vector<int64_t> pures;
    durations.reserve(tx.Attempts.size());
    pures.reserve(tx.Attempts.size());
    for (const auto& a : tx.Attempts) {
        durations.push_back(a.DurationUs);
        pures.push_back(a.LatencyPureUs);
    }
    tx.DurationUs = ComputeDurationStats(durations);
    tx.LatencyPureUs = ComputeDurationStats(pures);
}

void PrintAttempt(
    int index,
    int total,
    int repeats,
    const TDebugTxDesc& tx,
    const TDebugAttempt& attempt)
{
    const char* tag = "[OK]";
    if (attempt.Status == "failed") {
        tag = "[Failed]";
    } else if (attempt.Status == "user_aborted") {
        tag = "[UserAborted]";
    }
    std::cout << "Debug [" << index << "/" << total << "] " << tx.JsonName
              << " " << attempt.N << "/" << repeats
              << " " << attempt.DurationUs << " us (pure " << attempt.LatencyPureUs
              << " us) " << tag;
    if (!attempt.Error.empty() && attempt.Status != "ok") {
        std::cout << ": " << attempt.Error;
    }
    std::cout << std::endl;
}

void PrintTxSummary(const TDebugTxReport& tx, int repeats) {
    std::cout << "Debug " << tx.Type << " summary: first=" << tx.DurationUs.First
              << " us min=" << tx.DurationUs.Min
              << " us max=" << tx.DurationUs.Max
              << " us avg=" << tx.DurationUs.Avg << " us";
    if (tx.DurationUs.RestAvg.has_value()) {
        std::cout << " rest_avg=" << *tx.DurationUs.RestAvg << " us";
    }
    std::cout << " (ok=" << tx.Ok
              << " failed=" << tx.Failed
              << " user_aborted=" << tx.UserAborted
              << " repeats=" << repeats << ")" << std::endl;
}

TFuture<TDebugReport> RunDebugLoop(const TDebugProbeRequest request, ITaskQueue& taskQueue) {
    co_await TTaskReady(taskQueue, 0);

    TDebugReport report;
    report.RunId = request.RunId;
    report.Instance = request.Instance;
    report.Repeats = request.Repeats;
    report.WarehouseID = request.WarehouseID;
    report.WarehouseCount = request.WarehouseCount;
    report.DistrictID = request.DistrictID;

    if (request.SessionFactory == nullptr || request.ErrorClassifier == nullptr) {
        throw std::runtime_error("debug probe requires a session factory and error classifier");
    }
    if (request.Repeats <= 0) {
        throw std::runtime_error("debug --repeats must be greater than zero");
    }

    const int total = request.Repeats * static_cast<int>(kDebugTransactions.size());
    int index = 0;

    TTransactionContext context{
        0,
        request.WarehouseID,
        request.DistrictID,
        request.WarehouseCount,
        taskQueue,
        0,
        request.NewOrderRemoteWarehousePercent,
        request.PaymentRemoteWarehousePercent,
        {}};

    auto stopToken = GetGlobalInterruptSource().get_token();

    for (const auto& txDesc : kDebugTransactions) {
        TDebugTxReport txReport;
        txReport.Type = txDesc.JsonName;

        for (int n = 1; n <= request.Repeats; ++n) {
            ++index;
            TDebugAttempt attempt;
            attempt.N = n;

            if (stopToken.stop_requested()) {
                attempt.Status = "failed";
                attempt.Error = "interrupted";
                txReport.Failed += 1;
                txReport.Attempts.push_back(attempt);
                PrintAttempt(index, total, request.Repeats, txDesc, attempt);
                continue;
            }

            context.FixedInputs.reset();
            std::chrono::microseconds latencyPure{0};
            const auto start = std::chrono::steady_clock::now();

            co_await TTaskHasInflight(taskQueue, 0);
            bool inflight = true;
            auto releaseInflight = [&]() {
                if (inflight) {
                    taskQueue.DecInflight();
                    inflight = false;
                }
            };

            try {
                std::unique_ptr<ITpccSession> session;
                while (true) {
                    session = co_await TSuspendWithFuture(
                        request.SessionFactory->WaitCreateSession(),
                        context.TaskQueue, context.TerminalID);
                    if (session) {
                        break;
                    }
                    if (stopToken.stop_requested()) {
                        break;
                    }
                    co_await TSuspend(taskQueue, 0, std::chrono::milliseconds(1));
                }
                if (!session) {
                    attempt.Status = "failed";
                    attempt.Error = "no session";
                    txReport.Failed += 1;
                } else {
                    auto tx = co_await TSuspendWithFuture(
                        session->Begin(request.Isolation),
                        context.TaskQueue, context.TerminalID);
                    auto future = txDesc.TaskFunc(context, latencyPure, *tx);
                    auto result = co_await TSuspendWithFuture(
                        std::move(future), context.TaskQueue, context.TerminalID);
                    attempt.LatencyPureUs = latencyPure.count();
                    if (result) {
                        attempt.Status = "ok";
                        txReport.Ok += 1;
                    } else {
                        attempt.Status = "failed";
                        attempt.Error = "transaction returned false";
                        txReport.Failed += 1;
                    }
                }
            } catch (const TUserAbortedException&) {
                attempt.Status = "user_aborted";
                attempt.LatencyPureUs = latencyPure.count();
                txReport.UserAborted += 1;
            } catch (const TClassifiedError& ex) {
                attempt.Status = "failed";
                attempt.Error = ex.what();
                attempt.LatencyPureUs = latencyPure.count();
                txReport.Failed += 1;
            } catch (const std::exception& ex) {
                const EErrorClass cls = request.ErrorClassifier->ClassifyException(ex);
                attempt.Status = "failed";
                attempt.Error = ex.what();
                attempt.LatencyPureUs = latencyPure.count();
                txReport.Failed += 1;
                (void)cls;
            }

            const auto end = std::chrono::steady_clock::now();
            attempt.DurationUs = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            releaseInflight();
            txReport.Attempts.push_back(attempt);
            PrintAttempt(index, total, request.Repeats, txDesc, attempt);
        }

        FinalizeTxReport(txReport);
        PrintTxSummary(txReport, request.Repeats);
        if (txReport.Failed > 0) {
            report.Ok = false;
        }
        report.Transactions.push_back(std::move(txReport));
    }

    co_return report;
}

} // anonymous

const char* TransactionTypeJsonName(ETransactionType type) {
    switch (type) {
        case ETransactionType::NewOrder: return "new_order";
        case ETransactionType::Delivery: return "delivery";
        case ETransactionType::OrderStatus: return "order_status";
        case ETransactionType::Payment: return "payment";
        case ETransactionType::StockLevel: return "stock_level";
        default: return "unknown";
    }
}

TDurationStats ComputeDurationStats(const std::vector<int64_t>& samples) {
    TDurationStats stats;
    if (samples.empty()) {
        return stats;
    }
    stats.First = samples.front();
    stats.Min = samples.front();
    stats.Max = samples.front();
    int64_t sum = 0;
    for (int64_t v : samples) {
        stats.Min = std::min(stats.Min, v);
        stats.Max = std::max(stats.Max, v);
        sum += v;
    }
    stats.Avg = sum / static_cast<int64_t>(samples.size());
    if (samples.size() >= 2) {
        const int64_t rest = sum - samples.front();
        stats.RestAvg = rest / static_cast<int64_t>(samples.size() - 1);
    }
    return stats;
}

int TDebugReport::FailedCount() const {
    int n = 0;
    for (const auto& tx : Transactions) {
        n += tx.Failed;
    }
    return n;
}

std::string DebugReportToJson(const TDebugReport& report) {
    Json transactions = Json::array();
    for (const auto& tx : report.Transactions) {
        Json attempts = Json::array();
        for (const auto& a : tx.Attempts) {
            Json row = {
                {"n", a.N},
                {"status", a.Status},
                {"duration_us", a.DurationUs},
                {"latency_pure_us", a.LatencyPureUs},
            };
            if (!a.Error.empty()) {
                row["error"] = a.Error;
            }
            attempts.push_back(std::move(row));
        }
        transactions.push_back({
            {"type", tx.Type},
            {"ok", tx.Ok},
            {"failed", tx.Failed},
            {"user_aborted", tx.UserAborted},
            {"duration_us", DurationStatsJson(tx.DurationUs)},
            {"latency_pure_us", DurationStatsJson(tx.LatencyPureUs)},
            {"attempts", std::move(attempts)},
        });
    }
    Json j = {
        {"schema_version", report.SchemaVersion},
        {"role", report.Role},
        {"run_id", report.RunId},
        {"instance", report.Instance},
        {"repeats", report.Repeats},
        {"warehouse_id", report.WarehouseID},
        {"warehouse_count", report.WarehouseCount},
        {"district_id", report.DistrictID},
        {"ok", report.Ok},
        {"failed", report.FailedCount()},
        {"transactions", std::move(transactions)},
    };
    return j.dump(2);
}

void WriteDebugReportJson(const std::string& path, const TDebugReport& report) {
    const auto parent = fs::path(path).parent_path();
    if (!parent.empty()) {
        fs::create_directories(parent);
    }
    const std::string tmp = path + ".tmp";
    {
        std::ofstream out(tmp);
        if (!out) {
            throw std::runtime_error("failed to write " + tmp);
        }
        out << DebugReportToJson(report);
        out << '\n';
    }
    fs::rename(tmp, path);
}

TDebugReport RunDebugProbe(const TDebugProbeRequest& request) {
    signal(SIGINT, InterruptHandler);
    signal(SIGTERM, InterruptHandler);

    const int repeats = request.Repeats > 0 ? request.Repeats : kDefaultDebugRepeats;
    TDebugProbeRequest req = request;
    req.Repeats = repeats;

    std::cout << "Debug: sequential probe of each TPC-C transaction type "
              << repeats << " times (warehouse=" << req.WarehouseID
              << ", district=" << req.DistrictID
              << ", scale=" << req.WarehouseCount << ")" << std::endl;

    auto taskQueue = CreateTaskQueue(1, 1, 8, 8);
    taskQueue->Run();
    TDebugReport report;
    try {
        auto future = RunDebugLoop(req, *taskQueue);
        report = future.Get();
    } catch (...) {
        taskQueue->WakeupAndNeverSleep();
        taskQueue->Join();
        throw;
    }
    taskQueue->WakeupAndNeverSleep();
    taskQueue->Join();
    return report;
}

int RunOrchestratedDebug(
    const TRunConfigDocument& doc,
    const std::string& instance,
    int repeats,
    std::function<TDebugReport(const TRunConfigDocument&, TDebugProbeRequest)> runProbe)
{
    if (repeats <= 0) {
        throw std::runtime_error("debug --repeats must be greater than zero");
    }

    const std::string instanceDir = InstanceWorkDir(doc, "debug", instance);
    EnsureInstanceDir(instanceDir);
    const auto paths = MakeArtifactPaths(instanceDir);
    const std::string nonce = GenerateInstanceNonce();
    WriteProcessJson(paths, doc, instance, "debug", static_cast<int>(::getpid()), nonce);

    TDebugProbeRequest req;
    req.WarehouseID = 1;
    req.WarehouseCount = doc.ScaleWarehouses > 0 ? static_cast<size_t>(doc.ScaleWarehouses) : 1;
    req.DistrictID = 1;
    req.NewOrderRemoteWarehousePercent = doc.Workload.NewOrderRemoteWarehousePercent;
    req.PaymentRemoteWarehousePercent = doc.Workload.PaymentRemoteWarehousePercent;
    req.Repeats = repeats;
    req.RunId = doc.RunId;
    req.Instance = instance;
    if (!doc.WorkerAssignments.empty() && !doc.WorkerAssignments[0].WarehouseRanges.empty()) {
        req.WarehouseID = static_cast<size_t>(doc.WorkerAssignments[0].WarehouseRanges[0].Start);
    }

    int exitCode = 1;
    try {
        auto report = runProbe(doc, req);
        report.RunId = doc.RunId;
        report.Instance = instance;
        WriteDebugReportJson(paths.ResultJson, report);
        const std::string probePath = doc.RunDir + "/debug/probe.json";
        WriteDebugReportJson(probePath, report);
        LOG_I("Debug report written to " << probePath);
        std::cout << "Debug report written to " << probePath << std::endl;
        exitCode = report.Ok ? 0 : 1;
    } catch (const std::exception& ex) {
        LOG_E("Debug probe failed: " << ex.what());
        exitCode = 1;
    }

    WriteArtifactManifest(paths, instance, nonce, exitCode);
    return exitCode;
}

} // namespace NTpcc
