#include "terminal.h"
#include <coro_traits.h>
#include <log.h>
#include "transactions.h"
#include "tpcc_session.h"
#include "pg_error_classifier.h"
#include <domain_util.h>
#include <constants.h>
#include <rng.h>
#include <think_time.h>

#include <array>
#include <optional>
#include <pqxx/pqxx>

namespace NTpcc {

namespace {

struct TTerminalTransaction {
    using TTaskFunc = TFuture<bool> (*)(TTransactionContext&, std::chrono::microseconds&, ITpccTransaction&);

    std::string Name;
    double Weight;
    TTaskFunc TaskFunc;
    std::chrono::milliseconds KeyingTime{0};
    std::chrono::milliseconds ThinkTime{0};
};

std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT> BuildTransactions(const TWorkloadConfig& workload) {
    std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT> transactions{};

    auto fill = [&](ETransactionType type, const char* name, TTerminalTransaction::TTaskFunc fn) {
        const auto& w = workload.PerTx[static_cast<size_t>(type)];
        transactions[static_cast<size_t>(type)] = {
            name,
            w.Weight,
            fn,
            std::chrono::milliseconds(w.KeyingTimeMs),
            std::chrono::milliseconds(w.ThinkTimeMs),
        };
    };

    fill(ETransactionType::NewOrder, "NewOrder", &GetNewOrderTask);
    fill(ETransactionType::Delivery, "Delivery", &GetDeliveryTask);
    fill(ETransactionType::OrderStatus, "OrderStatus", &GetOrderStatusTask);
    fill(ETransactionType::Payment, "Payment", &GetPaymentTask);
    fill(ETransactionType::StockLevel, "StockLevel", &GetStockLevelTask);
    return transactions;
}

size_t ChooseRandomTransactionIndex(const std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT>& transactions) {
    double totalWeight = 0.0;
    for (const auto& tx : transactions) {
        totalWeight += tx.Weight;
    }
    if (totalWeight <= 0.0) {
        return 0;
    }

    double randomValue = RandomNumber(0, static_cast<size_t>(totalWeight * 100)) / 100.0;
    double cumulativeWeight = 0.0;

    for (size_t i = 0; i < transactions.size(); ++i) {
        cumulativeWeight += transactions[i].Weight;
        if (randomValue <= cumulativeWeight) {
            return i;
        }
    }

    return transactions.size() - 1;
}

size_t EffectiveRetryMaxAttempts(size_t configured) {
    // run-config uses 0 as "unset" → default 4 total attempts.
    return configured == 0 ? 4 : configured;
}

bool ShouldRetryClass(EErrorClass cls, bool retryAmbiguousCommit) {
    if (MayBlindRetry(cls)) {
        return true;
    }
    return cls == EErrorClass::AmbiguousCommit && retryAmbiguousCommit;
}

} // anonymous

TTerminal::TTerminal(size_t terminalID,
                     size_t warehouseID,
                     size_t districtID,
                     size_t warehouseCount,
                     ITaskQueue& taskQueue,
                     PgConnectionPool* connectionPool,
                     bool noDelays,
                     std::stop_token stopToken,
                     TPhaseController& phaseController,
                     std::shared_ptr<TTerminalStats>& stats,
                     const TWorkloadConfig& workload,
                     int simulateTransactionMs,
                     int simulateTransactionSelect1,
                     size_t retryMaxAttempts,
                     bool retryAmbiguousCommit,
                     EThinkTimeDistribution thinkTimeDistribution)
    : TaskQueue(taskQueue)
    , ConnectionPool(connectionPool)
    , Context{terminalID, warehouseID, districtID, warehouseCount, taskQueue,
              simulateTransactionMs, simulateTransactionSelect1, {}}
    , NoDelays(noDelays)
    , StopToken(stopToken)
    , PhaseController(phaseController)
    , Stats(stats)
    , Workload(workload)
    , RetryMaxAttempts(EffectiveRetryMaxAttempts(retryMaxAttempts))
    , RetryAmbiguousCommit(retryAmbiguousCommit)
    , ThinkTimeDistribution(thinkTimeDistribution)
{}

void TTerminal::Start() {
    if (!Started) {
        Run();
        Started = true;
    }
}

TFuture<void> TTerminal::Run() {
    co_await TTaskReady(TaskQueue, Context.TerminalID);

    LOG_D("Terminal {} started", Context.TerminalID);

    TPgErrorClassifier classifier;
    const auto transactions = BuildTransactions(Workload);

    while (!StopToken.stop_requested()) {
        if (!PhaseController.MayAdmit()) {
            // Only Prepare waits for ramp_start. Past MeasurementEnd (absolute),
            // Drain, or Stop must exit even if Tick() has not published Drain yet.
            if (PhaseController.Phase() == ERunPhase::Prepare) {
                co_await TSuspend(TaskQueue, Context.TerminalID, std::chrono::milliseconds(10));
                continue;
            }
            break;
        }

        const bool simulationMode =
            Context.SimulateTransactionMs > 0 || Context.SimulateTransactionSelect1 > 0;

        size_t txIndex = simulationMode ? 0 : ChooseRandomTransactionIndex(transactions);
        const char* txName = simulationMode ? "Simulation" : transactions[txIndex].Name.c_str();
        const auto txType = static_cast<ETransactionType>(txIndex);

        if (!NoDelays && !simulationMode) {
            auto& transaction = transactions[txIndex];
            LOG_T("Terminal {} keying time for {}: {}ms",
                Context.TerminalID, transaction.Name, transaction.KeyingTime.count());
            co_await TSuspend(TaskQueue, Context.TerminalID, transaction.KeyingTime);
            if (StopToken.stop_requested()) break;
            if (!PhaseController.MayAdmit()) {
                continue;
            }
        }

        // latencyFull and the §5.4.2 start boundary share this instant (including
        // any inflight-slot wait that follows).
        auto startTime = std::chrono::steady_clock::now();
        const auto startWall = std::chrono::system_clock::now();
        co_await TTaskHasInflight(TaskQueue, Context.TerminalID);
        if (StopToken.stop_requested() || !PhaseController.MayAdmit()) {
            TaskQueue.DecInflight();
            break;
        }

        LOG_T("Terminal {} starting {} transaction", Context.TerminalID, txName);

        auto startTimeTransaction = std::chrono::steady_clock::now();
        std::chrono::microseconds latencyPure{0};
        bool fatal = false;

        // Reset so each business transaction gets fresh inputs; retries reuse FixedInputs.
        Context.FixedInputs.reset();

        // TPC-C §5.4.2: count only when response-time start and end both lie
        // within the absolute measurement interval from the phase schedule.
        auto shouldRecordMetrics = [&](std::chrono::system_clock::time_point endWall) {
            return PhaseController.CompletelyWithinMeasurement(startWall, endWall);
        };
        auto recordOk = [&](auto latencyTransaction, auto latencyFull, auto endWall) {
            if (shouldRecordMetrics(endWall)) {
                Stats->AddOK(txType, latencyTransaction, latencyFull, latencyPure);
            }
        };
        auto recordFailed = [&](auto endWall) {
            if (shouldRecordMetrics(endWall)) {
                Stats->IncFailed(txType);
            }
        };
        auto recordUserAborted = [&](auto latencyTransaction, auto latencyFull, auto endWall) {
            if (shouldRecordMetrics(endWall)) {
                Stats->AddUserAborted(txType, latencyTransaction, latencyFull, latencyPure);
            }
        };
        auto recordRetried = [&](auto endWall) {
            if (shouldRecordMetrics(endWall)) {
                Stats->IncRetried(txType);
            }
        };

        for (size_t attempt = 0; attempt < RetryMaxAttempts; ++attempt) {
            bool shouldRetry = false;
            try {
                std::optional<PgConnectionPool::SessionGuard> guard;
                if (ConnectionPool) {
                    guard.emplace(ConnectionPool->AcquireGuard());
                }

                PgSession dummySession;
                PgSession& session = guard ? **guard : dummySession;

                latencyPure = std::chrono::microseconds{0};
                if (simulationMode) {
                    auto future = GetSimulationTask(Context, latencyPure, session);
                    auto result = co_await TSuspendWithFuture(
                        std::move(future), Context.TaskQueue, Context.TerminalID);
                    auto endTime = std::chrono::steady_clock::now();
                    const auto endWall = std::chrono::system_clock::now();
                    auto latencyFull = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    auto latencyTransaction = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTimeTransaction);
                    if (result) {
                        recordOk(latencyTransaction, latencyFull, endWall);
                        LOG_T("Terminal {} {} succeeded", Context.TerminalID, txName);
                    } else {
                        recordFailed(endWall);
                        LOG_D("Terminal {} {} failed", Context.TerminalID, txName);
                    }
                } else {
                    TPgTpccSession tpccSession(session);
                    auto beginFuture = tpccSession.Begin(EIsolationLevel::RepeatableRead);
                    auto tx = co_await TSuspendWithFuture(
                        std::move(beginFuture), Context.TaskQueue, Context.TerminalID);
                    auto future = transactions[txIndex].TaskFunc(Context, latencyPure, *tx);
                    auto result = co_await TSuspendWithFuture(
                        std::move(future), Context.TaskQueue, Context.TerminalID);
                    auto endTime = std::chrono::steady_clock::now();
                    const auto endWall = std::chrono::system_clock::now();
                    auto latencyFull = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    auto latencyTransaction = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTimeTransaction);
                    if (result) {
                        recordOk(latencyTransaction, latencyFull, endWall);
                        LOG_T("Terminal {} {} succeeded", Context.TerminalID, txName);
                    } else {
                        recordFailed(endWall);
                        LOG_D("Terminal {} {} failed", Context.TerminalID, txName);
                    }
                }
            } catch (const TUserAbortedException&) {
                auto endTime = std::chrono::steady_clock::now();
                const auto endWall = std::chrono::system_clock::now();
                auto latencyFull = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTime);
                auto latencyTransaction = std::chrono::duration_cast<std::chrono::milliseconds>(
                    endTime - startTimeTransaction);
                recordUserAborted(latencyTransaction, latencyFull, endWall);
                LOG_T("Terminal {} {} user aborted", Context.TerminalID, txName);
            } catch (const TClassifiedError& ex) {
                if (StopToken.stop_requested()) {
                    LOG_D("Terminal {} {} interrupted during shutdown", Context.TerminalID, txName);
                    break;
                }

                const EErrorClass cls = ex.Class;
                const bool attemptsRemain = (attempt + 1) < RetryMaxAttempts;
                const auto endWall = std::chrono::system_clock::now();

                if (cls == EErrorClass::Integrity) {
                    LOG_E("Terminal {} integrity error in {}: {}", Context.TerminalID, txName, ex.what());
                    recordFailed(endWall);
                    fatal = true;
                } else if (cls == EErrorClass::Cancelled) {
                    LOG_D("Terminal {} {} cancelled: {}", Context.TerminalID, txName, ex.what());
                    break;
                } else if (ShouldRetryClass(cls, RetryAmbiguousCommit) && attemptsRemain) {
                    shouldRetry = true;
                    recordRetried(endWall);
                    LOG_D("Terminal {} {} classified retry {}/{}: {}",
                          Context.TerminalID, txName, attempt + 1, RetryMaxAttempts, ex.what());
                } else {
                    recordFailed(endWall);
                    if (cls == EErrorClass::AmbiguousCommit && !RetryAmbiguousCommit) {
                        LOG_E("Terminal {} {} ambiguous commit (blind retry disabled): {}",
                              Context.TerminalID, txName, ex.what());
                    } else if (!attemptsRemain && ShouldRetryClass(cls, RetryAmbiguousCommit)) {
                        LOG_D("Terminal {} {} retries exhausted: {}", Context.TerminalID, txName, ex.what());
                    } else {
                        LOG_E("Terminal {} classified error in {}: {}", Context.TerminalID, txName, ex.what());
                        if (cls == EErrorClass::Permanent) {
                            fatal = true;
                        }
                    }
                }
            } catch (const std::exception& ex) {
                if (StopToken.stop_requested()) {
                    LOG_D("Terminal {} {} interrupted during shutdown", Context.TerminalID, txName);
                    break;
                }

                const EErrorClass cls = classifier.ClassifyException(ex);
                const bool attemptsRemain = (attempt + 1) < RetryMaxAttempts;
                const auto endWall = std::chrono::system_clock::now();

                if (cls == EErrorClass::Integrity) {
                    LOG_E("Terminal {} integrity error in {}: {}", Context.TerminalID, txName, ex.what());
                    recordFailed(endWall);
                    fatal = true;
                } else if (cls == EErrorClass::Cancelled) {
                    LOG_D("Terminal {} {} cancelled: {}", Context.TerminalID, txName, ex.what());
                    break;
                } else if (ShouldRetryClass(cls, RetryAmbiguousCommit) && attemptsRemain) {
                    shouldRetry = true;
                    recordRetried(endWall);
                    LOG_D("Terminal {} {} classified {} ({})",
                          Context.TerminalID, txName,
                          static_cast<int>(cls), ex.what());
                    LOG_D("Terminal {} {} retry {}/{}",
                          Context.TerminalID, txName, attempt + 1, RetryMaxAttempts);
                } else {
                    recordFailed(endWall);
                    if (cls == EErrorClass::AmbiguousCommit && !RetryAmbiguousCommit) {
                        LOG_E("Terminal {} {} ambiguous commit (blind retry disabled): {}",
                              Context.TerminalID, txName, ex.what());
                    } else if (!attemptsRemain && ShouldRetryClass(cls, RetryAmbiguousCommit)) {
                        LOG_D("Terminal {} {} retries exhausted: {}", Context.TerminalID, txName, ex.what());
                    } else {
                        LOG_E("Terminal {} exception in {}: {}", Context.TerminalID, txName, ex.what());
                        if (cls == EErrorClass::Permanent) {
                            fatal = true;
                        }
                    }
                }
            }

            if (!shouldRetry) break;
        }

        TaskQueue.DecInflight();

        if (fatal) {
            RequestStopWithError();
            Done.store(true, std::memory_order_relaxed);
            co_return;
        }

        if (!NoDelays && !simulationMode && PhaseController.MayAdmit()) {
            auto& transaction = transactions[txIndex];
            const auto thinkTime = std::chrono::milliseconds(SampleThinkTimeMs(
                transaction.ThinkTime.count(), ThinkTimeDistribution));
            LOG_T("Terminal {} think time: {}ms (mean {}ms, {})",
                  Context.TerminalID,
                  thinkTime.count(),
                  transaction.ThinkTime.count(),
                  ThinkTimeDistributionToString(ThinkTimeDistribution));
            co_await TSuspend(TaskQueue, Context.TerminalID, thinkTime);
        }
    }

    LOG_D("Terminal {} stopped", Context.TerminalID);
    Done.store(true, std::memory_order_relaxed);
    co_return;
}

} // namespace NTpcc
