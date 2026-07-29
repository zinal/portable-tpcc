#include "terminal.h"
#include <coro_traits.h>
#include <log.h>
#include "transactions.h"
#include "tpcc_session.h"
#include "pg_error_classifier.h"
#include <domain_util.h>
#include <constants.h>
#include <rng.h>

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
    std::chrono::seconds KeyingTime;
    std::chrono::seconds ThinkTime;
};

static std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT> CreateTransactions() {
    std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT> transactions{};

    transactions[static_cast<size_t>(ETransactionType::NewOrder)] =
        {"NewOrder", NEW_ORDER_WEIGHT, &GetNewOrderTask, NEW_ORDER_KEYING_TIME, NEW_ORDER_THINK_TIME};
    transactions[static_cast<size_t>(ETransactionType::Delivery)] =
        {"Delivery", DELIVERY_WEIGHT, &GetDeliveryTask, DELIVERY_KEYING_TIME, DELIVERY_THINK_TIME};
    transactions[static_cast<size_t>(ETransactionType::OrderStatus)] =
        {"OrderStatus", ORDER_STATUS_WEIGHT, &GetOrderStatusTask, ORDER_STATUS_KEYING_TIME, ORDER_STATUS_THINK_TIME};
    transactions[static_cast<size_t>(ETransactionType::Payment)] =
        {"Payment", PAYMENT_WEIGHT, &GetPaymentTask, PAYMENT_KEYING_TIME, PAYMENT_THINK_TIME};
    transactions[static_cast<size_t>(ETransactionType::StockLevel)] =
        {"StockLevel", STOCK_LEVEL_WEIGHT, &GetStockLevelTask, STOCK_LEVEL_KEYING_TIME, STOCK_LEVEL_THINK_TIME};

    return transactions;
}

static std::array<TTerminalTransaction, TRANSACTION_TYPE_COUNT> Transactions = CreateTransactions();

static size_t ChooseRandomTransactionIndex() {
    double totalWeight = 0.0;
    for (const auto& tx : Transactions) {
        totalWeight += tx.Weight;
    }

    double randomValue = RandomNumber(0, static_cast<size_t>(totalWeight * 100)) / 100.0;
    double cumulativeWeight = 0.0;

    for (size_t i = 0; i < Transactions.size(); ++i) {
        cumulativeWeight += Transactions[i].Weight;
        if (randomValue <= cumulativeWeight) {
            return i;
        }
    }

    return Transactions.size() - 1;
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
                     size_t warehouseCount,
                     ITaskQueue& taskQueue,
                     PgConnectionPool* connectionPool,
                     bool noDelays,
                     std::stop_token stopToken,
                     TPhaseController& phaseController,
                     std::shared_ptr<TTerminalStats>& stats,
                     int simulateTransactionMs,
                     int simulateTransactionSelect1,
                     size_t retryMaxAttempts,
                     bool retryAmbiguousCommit)
    : TaskQueue(taskQueue)
    , ConnectionPool(connectionPool)
    , Context{terminalID, warehouseID, warehouseCount, taskQueue,
              simulateTransactionMs, simulateTransactionSelect1, {}}
    , NoDelays(noDelays)
    , StopToken(stopToken)
    , PhaseController(phaseController)
    , Stats(stats)
    , RetryMaxAttempts(EffectiveRetryMaxAttempts(retryMaxAttempts))
    , RetryAmbiguousCommit(retryAmbiguousCommit)
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

    while (!StopToken.stop_requested()) {
        if (!PhaseController.MayAdmit()) {
            if (PhaseController.Phase() == ERunPhase::Drain ||
                PhaseController.Phase() == ERunPhase::Stop)
            {
                break;
            }
            // Prepare: wait for ramp_start / admission.
            co_await TSuspend(TaskQueue, Context.TerminalID, std::chrono::milliseconds(10));
            continue;
        }

        const bool recordMetrics = PhaseController.MayRecord();
        const bool simulationMode =
            Context.SimulateTransactionMs > 0 || Context.SimulateTransactionSelect1 > 0;

        size_t txIndex = simulationMode ? 0 : ChooseRandomTransactionIndex();
        const char* txName = simulationMode ? "Simulation" : Transactions[txIndex].Name.c_str();
        const auto txType = static_cast<ETransactionType>(txIndex);

        if (!NoDelays && !simulationMode) {
            auto& transaction = Transactions[txIndex];
            LOG_T("Terminal {} keying time for {}: {}s",
                Context.TerminalID, transaction.Name, transaction.KeyingTime.count());
            co_await TSuspend(TaskQueue, Context.TerminalID, transaction.KeyingTime);
            if (StopToken.stop_requested()) break;
            if (!PhaseController.MayAdmit()) {
                continue;
            }
        }

        auto startTime = std::chrono::steady_clock::now();
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

        auto recordOk = [&](auto latencyTransaction, auto latencyFull) {
            if (recordMetrics) {
                Stats->AddOK(txType, latencyTransaction, latencyFull, latencyPure);
            }
        };
        auto recordFailed = [&]() {
            if (recordMetrics) {
                Stats->IncFailed(txType);
            }
        };
        auto recordUserAborted = [&]() {
            if (recordMetrics) {
                Stats->IncUserAborted(txType);
            }
        };
        auto recordRetried = [&]() {
            if (recordMetrics) {
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
                    auto latencyFull = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    auto latencyTransaction = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTimeTransaction);
                    if (result) {
                        recordOk(latencyTransaction, latencyFull);
                        LOG_T("Terminal {} {} succeeded", Context.TerminalID, txName);
                    } else {
                        recordFailed();
                        LOG_D("Terminal {} {} failed", Context.TerminalID, txName);
                    }
                } else {
                    TPgTpccSession tpccSession(session);
                    auto beginFuture = tpccSession.Begin(EIsolationLevel::RepeatableRead);
                    auto tx = co_await TSuspendWithFuture(
                        std::move(beginFuture), Context.TaskQueue, Context.TerminalID);
                    auto future = Transactions[txIndex].TaskFunc(Context, latencyPure, *tx);
                    auto result = co_await TSuspendWithFuture(
                        std::move(future), Context.TaskQueue, Context.TerminalID);
                    auto endTime = std::chrono::steady_clock::now();
                    auto latencyFull = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
                    auto latencyTransaction = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTimeTransaction);
                    if (result) {
                        recordOk(latencyTransaction, latencyFull);
                        LOG_T("Terminal {} {} succeeded", Context.TerminalID, txName);
                    } else {
                        recordFailed();
                        LOG_D("Terminal {} {} failed", Context.TerminalID, txName);
                    }
                }
            } catch (const TUserAbortedException&) {
                recordUserAborted();
                LOG_T("Terminal {} {} user aborted", Context.TerminalID, txName);
            } catch (const TClassifiedError& ex) {
                if (StopToken.stop_requested()) {
                    LOG_D("Terminal {} {} interrupted during shutdown", Context.TerminalID, txName);
                    break;
                }

                const EErrorClass cls = ex.Class;
                const bool attemptsRemain = (attempt + 1) < RetryMaxAttempts;

                if (cls == EErrorClass::Integrity) {
                    LOG_E("Terminal {} integrity error in {}: {}", Context.TerminalID, txName, ex.what());
                    recordFailed();
                    fatal = true;
                } else if (cls == EErrorClass::Cancelled) {
                    LOG_D("Terminal {} {} cancelled: {}", Context.TerminalID, txName, ex.what());
                    break;
                } else if (ShouldRetryClass(cls, RetryAmbiguousCommit) && attemptsRemain) {
                    shouldRetry = true;
                    recordRetried();
                    LOG_D("Terminal {} {} classified retry {}/{}: {}",
                          Context.TerminalID, txName, attempt + 1, RetryMaxAttempts, ex.what());
                } else {
                    recordFailed();
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

                if (cls == EErrorClass::Integrity) {
                    LOG_E("Terminal {} integrity error in {}: {}", Context.TerminalID, txName, ex.what());
                    recordFailed();
                    fatal = true;
                } else if (cls == EErrorClass::Cancelled) {
                    LOG_D("Terminal {} {} cancelled: {}", Context.TerminalID, txName, ex.what());
                    break;
                } else if (ShouldRetryClass(cls, RetryAmbiguousCommit) && attemptsRemain) {
                    shouldRetry = true;
                    recordRetried();
                    LOG_D("Terminal {} {} classified {} ({})",
                          Context.TerminalID, txName,
                          static_cast<int>(cls), ex.what());
                    LOG_D("Terminal {} {} retry {}/{}",
                          Context.TerminalID, txName, attempt + 1, RetryMaxAttempts);
                } else {
                    recordFailed();
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
            auto& transaction = Transactions[txIndex];
            LOG_T("Terminal {} think time: {}s", Context.TerminalID, transaction.ThinkTime.count());
            co_await TSuspend(TaskQueue, Context.TerminalID, transaction.ThinkTime);
        }
    }

    LOG_D("Terminal {} stopped", Context.TerminalID);
    Done.store(true, std::memory_order_relaxed);
    co_return;
}

} // namespace NTpcc
