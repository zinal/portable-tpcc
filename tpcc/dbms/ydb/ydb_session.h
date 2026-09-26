#pragma once

#include "ydb_driver.h"
#include "ydb_error_classifier.h"

#include <session.h>

#include <constants.h>

#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/params/params.h>
#include <ydb/public/sdk/cpp/include/ydb-cpp-sdk/client/query/client.h>

#include <array>
#include <memory>
#include <optional>
#include <string>

namespace NTpcc {

class TYdbTpccTransaction : public ITpccTransaction {
public:
    TYdbTpccTransaction(
        NYdb::NQuery::TSession session,
        std::string path,
        NYdb::NQuery::TTxSettings txSettings);

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
    TFuture<TFinalCommitResult> CommitAfterOperation(TOperationResult operation);
    TFuture<TOperationResult> RollbackThenFailOp(EErrorClass cls, std::string message);
    TFuture<TBatchResult> RollbackThenFailBatch(EErrorClass cls, std::string message);
    void ResetTxnState();
    TFuture<NYdb::NQuery::TExecuteQueryResult> ExecQuery(
        std::string query,
        std::optional<NYdb::TParams> params = std::nullopt,
        bool commit = false);

    NYdb::NQuery::TSession Session_;
    std::optional<NYdb::NQuery::TTransaction> Tx_;
    NYdb::NQuery::TTxSettings TxSettings_;
    std::string Path_;
    TYdbErrorClassifier Classifier_;
    bool Terminal_ = false;
    bool FinalCommitMode_ = false;
    std::optional<TUpdateCustomerPayment> PendingPaymentUpdate_;
    struct TDeliveryPrefetch {
        bool Loaded = false;
        int WarehouseID = 0;
        std::array<std::optional<int>, DISTRICT_COUNT> OldestOrderId{};
        std::array<std::optional<TDeliveryOrderInfo>, DISTRICT_COUNT> Info{};
    };
    TDeliveryPrefetch DeliveryPrefetch_;
};

class TYdbTpccSession : public ITpccSession {
public:
    TYdbTpccSession(TYdbConnection& connection, std::string path);

    TFuture<std::unique_ptr<ITpccTransaction>> Begin(EIsolationLevel isolation) override;

private:
    TYdbConnection& Connection_;
    std::string Path_;
};

class TYdbSessionFactory : public ISessionFactory {
public:
    explicit TYdbSessionFactory(TYdbConnection& connection);

    std::unique_ptr<ITpccSession> CreateSession() override;

private:
    TYdbConnection& Connection_;
};

} // namespace NTpcc
