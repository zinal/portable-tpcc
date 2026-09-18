#pragma once

#include "ob_connection_pool.h"
#include "ob_error_classifier.h"
#include "ob_session.h"
#include "ob_batch.h"

#include <session.h>

#include <array>
#include <memory>
#include <optional>
#include <vector>

namespace NTpcc {

class TObTpccTransaction : public ITpccTransaction {
public:
    explicit TObTpccTransaction(TObSession& session);

    TFuture<TOperationResult> Execute(const TSemanticOp& op) override;
    TFuture<TBatchResult> ExecuteBatch(const std::vector<TSemanticOp>& ops) override;
    TFuture<TFinalCommitResult> ExecuteFinalAndCommit(const TSemanticOp& op) override;
    TFuture<TCommitResult> Commit() override;
    TFuture<TCommitResult> Rollback() override;
    TFuture<TCommitResult> Cancel() override;

    TFuture<TOperationResult> ExecuteSelect1() override;

private:
    TFuture<TOperationResult> CatchOp(TFuture<TOperationResult> future);
    TFuture<TBatchResult> CatchBatch(TFuture<TBatchResult> future);
    TFuture<TFinalCommitResult> CatchFinal(TFuture<TFinalCommitResult> future, bool commitAttempted);
    TFuture<TBatchResult> ExecuteBatchSequentially(std::vector<TSemanticOp> ops);
    TFuture<TBatchResult> ExecuteStockBatch(const std::vector<TSemanticOp>& ops);
    TFuture<TBatchResult> ExecuteOrderLineBatch(const std::vector<TSemanticOp>& ops);
    TFuture<TBatchResult> ExecuteCompleteDeliveryBatch(const std::vector<TSemanticOp>& ops);
    TFuture<TBatchResult> ExecuteApplyDeliveryBatch(const std::vector<TSemanticOp>& ops);
    TFuture<TOperationResult> EnsureDeliveryPrefetch(int warehouseId);
    TOperationResult OldestFromCache(int districtId) const;
    TOperationResult DeliveryInfoFromCache(int districtId, int orderId) const;
    TFuture<TFinalCommitResult> FinishPayment(
        const TUpdateCustomerPayment& update,
        const TInsertPaymentHistory& history);
    TFuture<TFinalCommitResult> FinishApplyDelivery(const TApplyDeliveryToCustomer& apply);
    void ResetTxnState();

    TObSession& Session_;
    TObErrorClassifier Classifier_;
    bool Terminal_ = false;
    bool PaymentLocationApplied_ = false;
    std::optional<TUpdateCustomerPayment> PendingPaymentUpdate_;
    struct TDeliveryPrefetch {
        bool Loaded = false;
        int WarehouseID = 0;
        std::array<std::optional<int>, DISTRICT_COUNT> OldestOrderId{};
        std::array<std::optional<TDeliveryOrderInfo>, DISTRICT_COUNT> Info{};
    };
    TDeliveryPrefetch DeliveryPrefetch_;
};

class TObTpccSession : public ITpccSession {
public:
    explicit TObTpccSession(TObSession& session);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    TObSession& Session_;
};

class TObSessionFactory : public ISessionFactory {
public:
    explicit TObSessionFactory(TObConnectionPool& pool);

    std::unique_ptr<ITpccSession> CreateSession() override;
    std::unique_ptr<ITpccSession> TryCreateSession() override;
    TFuture<std::unique_ptr<ITpccSession>> WaitCreateSession() override;

private:
    TObConnectionPool& Pool_;
};

} // namespace NTpcc
